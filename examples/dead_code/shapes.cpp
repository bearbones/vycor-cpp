// Copyright (c) 2026 The vycor-cpp Authors
// Original author: Alex Mason
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "shapes.hpp"
#include <cmath>
#include <iostream>

// --- Shape (base) ---

void Shape::debug_print() const {
    std::cout << "[Shape] " << name() << " area=" << area() << "\n";
}

// --- Circle ---

double Circle::area() const {
    return 3.14159265358979 * radius_ * radius_;
}

std::string Circle::name() const {
    return "Circle";
}

void Circle::debug_print() const {
    std::cout << "[Circle] r=" << radius_ << " area=" << area() << "\n";
}

double Circle::circumference() const {
    return 2.0 * 3.14159265358979 * radius_;
}

// --- Triangle ---

double Triangle::area() const {
    return 0.5 * base_ * height_;
}

std::string Triangle::name() const {
    return "Triangle";
}

void Triangle::debug_print() const {
    std::cout << "[Triangle] b=" << base_ << " h=" << height_
              << " area=" << area() << "\n";
}

double Triangle::hypotenuse() const {
    return std::sqrt(base_ * base_ + height_ * height_);
}

// --- Square ---

double Square::area() const {
    return side_ * side_;
}

std::string Square::name() const {
    return "Square";
}

// Square deliberately does NOT override debug_print().

// --- Hexagon ---

double Hexagon::area() const {
    return 1.5 * std::sqrt(3.0) * side_ * side_;
}

std::string Hexagon::name() const {
    return "Hexagon";
}

void Hexagon::debug_print() const {
    std::cout << "[Hexagon] s=" << side_ << " area=" << area() << "\n";
}

// --- Free functions ---

void print_shape_info(const Shape& s) {
    std::cout << "Shape: " << s.name() << "\n";
    std::cout << "  area = " << s.area() << "\n";
    s.debug_print();
}

std::unique_ptr<Shape> make_shape(int kind) {
    switch (kind) {
    case 0:  return std::make_unique<Circle>(1.0);
    case 1:  return std::make_unique<Triangle>(3.0, 4.0);
    case 2:  return std::make_unique<Square>(2.0);
    default: return std::make_unique<Circle>(1.0);
    }
    // Note: Hexagon is never constructed here or anywhere else.
}
