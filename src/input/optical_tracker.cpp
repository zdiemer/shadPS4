// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "input/optical_tracker.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <functional>
#include <limits>
#include <vector>

#include <nlohmann/json.hpp>

namespace Input {
namespace {
using Vector = std::array<double, 3>;

double Dot(const Vector& a, const Vector& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

Vector TransposeMultiply(const std::array<double, 9>& rotation, const Vector& value) {
    Vector result{};
    for (size_t row = 0; row < 3; ++row) {
        for (size_t column = 0; column < 3; ++column) {
            result[row] += rotation[column * 3 + row] * value[column];
        }
    }
    return result;
}

struct Blob {
    double x{};
    double y{};
    size_t area{};
};

Vector ReadColour(const std::vector<u8>& image, size_t pixel) {
    const auto pair = (pixel & ~size_t{1}) * 2;
    const int y = image[pixel * 2] - 16;
    const int u = image[pair + 1] - 128;
    const int v = image[pair + 3] - 128;
    return {
        static_cast<double>(std::clamp((298 * y + 409 * v + 128) >> 8, 0, 255)),
        static_cast<double>(std::clamp((298 * y - 100 * u - 208 * v + 128) >> 8, 0, 255)),
        static_cast<double>(std::clamp((298 * y + 516 * u + 128) >> 8, 0, 255)),
    };
}

std::vector<Blob> FindBlobs(const std::vector<u8>& image, int width, int height,
                            const std::array<u8, 3>& colour) {
    const auto target_min = std::ranges::min(colour);
    Vector target{};
    for (size_t i = 0; i < target.size(); ++i) {
        target[i] = colour[i] - target_min;
    }
    const double target_norm = Dot(target, target);
    if (target_norm < 1024) {
        return {};
    }
    std::vector<u8> mask(static_cast<size_t>(width) * height);
    for (size_t pixel = 0; pixel < mask.size(); ++pixel) {
        if (image[pixel * 2] < 24) {
            continue;
        }
        auto rgb = ReadColour(image, pixel);
        const auto [minimum, maximum] = std::ranges::minmax(rgb);
        constexpr double MinimumLightBrightness = 120;
        if (maximum < MinimumLightBrightness) {
            continue;
        }
        mask[pixel] = 1;
        if (maximum - minimum < 32) {
            continue;
        }
        for (auto& value : rgb) {
            value -= minimum;
        }
        const double similarity = Dot(rgb, target);
        if (similarity > 0 && similarity * similarity > 0.9 * Dot(rgb, rgb) * target_norm) {
            mask[pixel] = 2;
        }
    }
    std::vector<Blob> blobs;
    std::vector<size_t> pending;
    for (size_t pixel = 0; pixel < mask.size(); ++pixel) {
        if (mask[pixel] != 2) {
            continue;
        }
        pending.clear();
        pending.push_back(pixel);
        mask[pixel] = 0;
        size_t colour_pixels = 1;
        double sum_x{}, sum_y{};
        Vector total_colour{};
        for (size_t next = 0; next < pending.size(); ++next) {
            const auto location = pending[next];
            const auto x = location % width;
            const auto y = location / width;
            sum_x += x;
            sum_y += y;
            const auto rgb = ReadColour(image, location);
            const auto minimum = std::ranges::min(rgb);
            for (size_t axis = 0; axis < total_colour.size(); ++axis) {
                total_colour[axis] += rgb[axis] - minimum;
            }
            const auto visit = [&](size_t neighbour) {
                if (mask[neighbour]) {
                    colour_pixels += mask[neighbour] == 2;
                    mask[neighbour] = 0;
                    pending.push_back(neighbour);
                }
            };
            if (x > 0) {
                visit(location - 1);
            }
            if (x + 1 < width) {
                visit(location + 1);
            }
            if (y > 0) {
                visit(location - width);
            }
            if (y + 1 < height) {
                visit(location + width);
            }
        }
        const double similarity = Dot(total_colour, target);
        if (colour_pixels >= 4 && pending.size() < mask.size() / 50 && similarity > 0 &&
            similarity * similarity > 0.85 * Dot(total_colour, total_colour) * target_norm) {
            blobs.push_back({sum_x / pending.size(), sum_y / pending.size(), pending.size()});
        }
    }
    std::ranges::sort(blobs, std::greater{}, &Blob::area);
    if (blobs.size() > 8) {
        blobs.resize(8);
    }
    return blobs;
}

Vector Unproject(const Blob& blob, const std::array<double, 4>& intrinsics,
                 const std::array<double, 5>& distortion) {
    const double target_x = (blob.x - intrinsics[2]) / intrinsics[0];
    const double target_y = (blob.y - intrinsics[3]) / intrinsics[1];
    double x = target_x, y = target_y;
    for (int i = 0; i < 8; ++i) {
        const double radius = x * x + y * y;
        const double radial =
            1 + radius * (distortion[0] + radius * (distortion[1] + radius * distortion[4]));
        const double dx = 2 * distortion[2] * x * y + distortion[3] * (radius + 2 * x * x);
        const double dy = distortion[2] * (radius + 2 * y * y) + 2 * distortion[3] * x * y;
        x = (target_x - dx) / radial;
        y = (target_y - dy) / radial;
    }
    const double length = std::sqrt(x * x + y * y + 1);
    return {x / length, y / length, 1 / length};
}

bool IsRotation(const std::array<double, 9>& matrix) {
    for (size_t i = 0; i < 3; ++i) {
        for (size_t j = 0; j < 3; ++j) {
            double product{};
            for (size_t k = 0; k < 3; ++k) {
                product += matrix[i * 3 + k] * matrix[j * 3 + k];
            }
            if (!std::isfinite(product) || std::abs(product - (i == j ? 1.0 : 0.0)) > 0.001) {
                return false;
            }
        }
    }
    const double determinant = matrix[0] * (matrix[4] * matrix[8] - matrix[5] * matrix[7]) -
                               matrix[1] * (matrix[3] * matrix[8] - matrix[5] * matrix[6]) +
                               matrix[2] * (matrix[3] * matrix[7] - matrix[4] * matrix[6]);
    return std::abs(determinant - 1) < 0.001;
}
} // namespace

bool OpticalTracker::LoadCalibration(const std::filesystem::path& path) {
    calibrated = false;
    cached_sequence = 0;
    cached_position.reset();
    tracked_position.reset();
    pending_position.reset();
    pending_frames = 0;
    std::ifstream input{path};
    if (!input) {
        return false;
    }
    try {
        const auto data = nlohmann::json::parse(input);
        if (data.at("version").get<int>() != 1) {
            return false;
        }
        width = data.at("image_width").get<int>();
        height = data.at("image_height").get<int>();
        intrinsics = data.at("intrinsics").get<decltype(intrinsics)>();
        distortion = data.at("distortion").get<decltype(distortion)>();
        rotation = data.at("right_from_left_rotation").get<decltype(rotation)>();
        translation = data.at("right_from_left_translation").get<decltype(translation)>();
        if (width != 1280 || height != 800 || !IsRotation(rotation)) {
            return false;
        }
        for (size_t eye = 0; eye < 2; ++eye) {
            for (const auto value : intrinsics[eye]) {
                if (!std::isfinite(value)) {
                    return false;
                }
            }
            for (const auto value : distortion[eye]) {
                if (!std::isfinite(value)) {
                    return false;
                }
            }
            if (intrinsics[eye][0] < 100 || intrinsics[eye][1] < 100 || intrinsics[eye][0] > 4000 ||
                intrinsics[eye][1] > 4000 || intrinsics[eye][2] < 0 ||
                intrinsics[eye][2] >= width || intrinsics[eye][3] < 0 ||
                intrinsics[eye][3] >= height) {
                return false;
            }
        }
        const double baseline = std::sqrt(Dot(translation, translation));
        if (!std::isfinite(baseline) || baseline < 0.03 || baseline > 0.15) {
            return false;
        }
        calibrated = true;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
    return true;
}

std::optional<std::array<float, 3>> OpticalTracker::Locate(const StereoCameraFrame& frame,
                                                           const std::array<u8, 3>& colour) {
    if (!calibrated || frame.width != width || frame.height != height ||
        std::chrono::steady_clock::now() - frame.sample_time > std::chrono::milliseconds{100}) {
        return std::nullopt;
    }
    if (frame.sequence == cached_sequence && colour == cached_colour) {
        return cached_position;
    }
    if (colour != cached_colour) {
        tracked_position.reset();
        pending_position.reset();
        pending_frames = 0;
    }
    cached_sequence = frame.sequence;
    cached_colour = colour;
    cached_position.reset();
    std::array<std::vector<Blob>, 2> blobs;
    for (size_t eye = 0; eye < 2; ++eye) {
        if (frame.images[eye].size() != static_cast<size_t>(width) * height * 2) {
            return std::nullopt;
        }
        blobs[eye] = FindBlobs(frame.images[eye], width, height, colour);
    }
    auto right_origin = TransposeMultiply(rotation, translation);
    for (auto& value : right_origin) {
        value = -value;
    }
    double best_score = std::numeric_limits<double>::max();
    double second_score = best_score;
    const double elapsed = std::chrono::duration<double>(frame.sample_time - tracked_time).count();
    const bool continuous = tracked_position && elapsed > 0 && elapsed <= 0.25;
    const double maximum_displacement = 0.03 + 5.0 * elapsed;
    for (const auto& left : blobs[0]) {
        for (const auto& right : blobs[1]) {
            const double area_ratio = static_cast<double>(left.area) / right.area;
            if (area_ratio < 0.4 || area_ratio > 2.5) {
                continue;
            }
            const auto a = Unproject(left, intrinsics[0], distortion[0]);
            const auto b =
                TransposeMultiply(rotation, Unproject(right, intrinsics[1], distortion[1]));
            const double ab = Dot(a, b), ad = Dot(a, right_origin), bd = Dot(b, right_origin);
            const double denominator = 1 - ab * ab;
            if (denominator < 0.00001) {
                continue;
            }
            const double distance_left = (ad - ab * bd) / denominator;
            const double distance_right = (ab * ad - bd) / denominator;
            if (distance_left < 0.1 || distance_left > 5 || distance_right < 0.1 ||
                distance_right > 5) {
                continue;
            }
            Vector point{}, difference{};
            for (size_t axis = 0; axis < 3; ++axis) {
                const double l = a[axis] * distance_left;
                const double r = right_origin[axis] + b[axis] * distance_right;
                point[axis] = (l + r) * 0.5;
                difference[axis] = l - r;
            }
            const double gap = std::sqrt(Dot(difference, difference));
            const double error = gap * std::max(intrinsics[0][0], intrinsics[1][0]) /
                                 std::min(distance_left, distance_right);
            if (!std::isfinite(error) || gap > 0.02 || error > 3) {
                continue;
            }
            double score = error + std::abs(std::log(area_ratio));
            if (continuous) {
                Vector movement{};
                for (size_t axis = 0; axis < 3; ++axis) {
                    movement[axis] = point[axis] - tracked_position->at(axis);
                }
                const double displacement = std::sqrt(Dot(movement, movement));
                if (displacement > maximum_displacement) {
                    continue;
                }
                score += displacement / maximum_displacement;
            }
            if (score < best_score) {
                second_score = best_score;
                best_score = score;
                cached_position = {static_cast<float>(point[0]), static_cast<float>(point[1]),
                                   static_cast<float>(point[2])};
            } else {
                second_score = std::min(second_score, score);
            }
        }
    }
    if (second_score - best_score < 0.25) {
        cached_position.reset();
    }
    if (!cached_position) {
        pending_position.reset();
        pending_frames = 0;
        return std::nullopt;
    }
    if (tracked_position && !continuous) {
        const double pending_elapsed =
            std::chrono::duration<double>(frame.sample_time - pending_time).count();
        Vector movement{};
        if (pending_position) {
            for (size_t axis = 0; axis < 3; ++axis) {
                movement[axis] = cached_position->at(axis) - pending_position->at(axis);
            }
        }
        if (!pending_position || pending_elapsed <= 0 || pending_elapsed > 0.1 ||
            std::sqrt(Dot(movement, movement)) > 0.03 + 5.0 * pending_elapsed) {
            pending_frames = 0;
        }
        pending_position = cached_position;
        pending_time = frame.sample_time;
        if (++pending_frames < 3) {
            cached_position.reset();
            return std::nullopt;
        }
    }
    tracked_position = cached_position;
    tracked_time = frame.sample_time;
    pending_position.reset();
    pending_frames = 0;
    return cached_position;
}

} // namespace Input
