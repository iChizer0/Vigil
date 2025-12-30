#include <iostream>
#include <string>

#include "vigil/types.hpp"

using namespace vigil::types;

// Use a small mask size for easier visual verification
using TestMask = Mask<16, 16>;

void test_bbox_mask()
{
    std::cout << "=== BoundingBox Mask Test ===" << std::endl;

    // Create a bounding box in the center (4,4) to (11,11)
    BoundingBox<size_t> bbox { .top_left = { 4, 4 }, .bottom_right = { 11, 11 } };
    auto mask = TestMask::from(bbox);

    if (mask)
    {
        std::cout << "BoundingBox (4,4)-(11,11) on 16x16 mask:" << std::endl;
        std::cout << "Area: " << mask->area() << std::endl;
        std::cout << "Encoded: " << mask->encoded() << std::endl;
        std::cout << std::endl;
    }
}

void test_triangle_mask()
{
    std::cout << "=== Triangle Mask Test ===" << std::endl;

    // Create a triangle: top-center, bottom-left, bottom-right
    Polygon<size_t> triangle;
    triangle.points = {
        { 8, 2 },   // top-center
        { 2, 13 },  // bottom-left
        { 13, 13 }, // bottom-right
    };

    auto mask = TestMask::from(triangle);
    if (mask)
    {
        std::cout << "Triangle polygon on 16x16 mask:" << std::endl;
        std::cout << "Area: " << mask->area() << std::endl;
        std::cout << "Encoded: " << mask->encoded() << std::endl;
        std::cout << std::endl;
    }
}

void test_diamond_mask()
{
    std::cout << "=== Diamond Mask Test ===" << std::endl;

    // Create a diamond shape
    Polygon<size_t> diamond;
    diamond.points = {
        { 8, 1 },  // top
        { 14, 8 }, // right
        { 8, 14 }, // bottom
        { 1, 8 },  // left
    };

    auto mask = TestMask::from(diamond);
    if (mask)
    {
        std::cout << "Diamond polygon on 16x16 mask:" << std::endl;
        std::cout << "Area: " << mask->area() << std::endl;
        std::cout << "Encoded: " << mask->encoded() << std::endl;
        std::cout << std::endl;
    }
}

void test_float_polygon_mask()
{
    std::cout << "=== Float Polygon Mask Test ===" << std::endl;

    // Create a polygon using normalized float coordinates [0.0, 1.0]
    Polygon<float> polygon;
    polygon.points = {
        { 0.5f, 0.1f }, // top-center
        { 0.9f, 0.5f }, // right
        { 0.5f, 0.9f }, // bottom-center
        { 0.1f, 0.5f }, // left
    };

    auto mask = TestMask::from(polygon);
    if (mask)
    {
        std::cout << "Float diamond (normalized coords) on 16x16 mask:" << std::endl;
        std::cout << "Area: " << mask->area() << std::endl;
        std::cout << "Encoded: " << mask->encoded() << std::endl;
        std::cout << std::endl;
    }
}

void test_full_mask()
{
    std::cout << "=== Full Mask Test ===" << std::endl;

    auto mask = TestMask::from(true);
    if (mask)
    {
        std::cout << "Full mask (all bits set) on 16x16 mask:" << std::endl;
        std::cout << "Area: " << mask->area() << std::endl;
        std::cout << "Encoded: " << mask->encoded() << std::endl;
        std::cout << std::endl;
    }
}

void test_empty_mask()
{
    std::cout << "=== Empty Mask Test ===" << std::endl;

    auto mask = TestMask::from(false);
    if (mask)
    {
        std::cout << "Empty mask (all bits clear) on 16x16 mask:" << std::endl;
        std::cout << "Area: " << mask->area() << std::endl;
        std::cout << "Encoded: " << mask->encoded() << std::endl;
        std::cout << std::endl;
    }
}

int main()
{
    std::cout << "Mask Encoding Test" << std::endl;
    std::cout << "sizeof(size_t) = " << sizeof(size_t) << " bytes" << std::endl;
    std::cout << std::endl;

    test_bbox_mask();
    test_triangle_mask();
    test_diamond_mask();
    test_float_polygon_mask();
    test_full_mask();
    test_empty_mask();

    return 0;
}
