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

#pragma once
#include <memory>
#include <string>

// --- Virtual dispatch scenarios for dead code analysis ---
//
// Liveness depends on whether a derived type is *provably* constructed
// on a path that reaches a virtual dispatch site.
//
// Key scenarios:
//   Circle, Triangle — constructed directly in main, proven alive
//   Square           — constructable inside make_shape(), but no proven
//                      dispatch path (optimistically alive only)
//   Hexagon          — never constructed anywhere (dead in both modes)

class Shape {
public:
    virtual ~Shape() = default;
    virtual double area() const = 0;
    virtual std::string name() const = 0;

    // Default implementation. Only dispatched for types that don't
    // override it. Square is the only such type, and Square is itself
    // only optimistically alive — making this a cascading opt/pess case.
    virtual void debug_print() const;
};

class Circle : public Shape {
public:
    explicit Circle(double r) : radius_(r) {}
    double area() const override;
    std::string name() const override;
    void debug_print() const override;

    // Non-virtual, only alive if Circle is specifically used.
    double circumference() const;

private:
    double radius_;
};

class Triangle : public Shape {
public:
    Triangle(double b, double h) : base_(b), height_(h) {}
    double area() const override;
    std::string name() const override;
    void debug_print() const override;

    // Never called anywhere — dead in both modes.
    double hypotenuse() const;

private:
    double base_;
    double height_;
};

class Square : public Shape {
public:
    explicit Square(double s) : side_(s) {}
    double area() const override;
    std::string name() const override;
    // Deliberately does NOT override debug_print() — uses base default.

private:
    double side_;
};

class Hexagon : public Shape {
public:
    explicit Hexagon(double s) : side_(s) {}
    double area() const override;
    std::string name() const override;
    void debug_print() const override;

private:
    double side_;
};

// Accepts Shape& and calls virtual methods — triggers dispatch.
void print_shape_info(const Shape& s);

// Factory that *can* construct any shape type. Returns Shape*.
// The caller cannot prove which derived type it gets back, so
// all overrides of the returned type are only "optimistically alive."
std::unique_ptr<Shape> make_shape(int kind);
