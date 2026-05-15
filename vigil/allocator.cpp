#include "allocator.hpp"

#include <cstddef>
#include <new>

void *operator new(std::size_t size)
{
    return vigil::allocator::allocate(size);
}

void *operator new[](std::size_t size)
{
    return vigil::allocator::allocate(size);
}

void *operator new(std::size_t size, const std::nothrow_t &) noexcept
{
    return vigil::allocator::allocate_nothrow(size);
}

void *operator new[](std::size_t size, const std::nothrow_t &) noexcept
{
    return vigil::allocator::allocate_nothrow(size);
}

void operator delete(void *ptr) noexcept
{
    vigil::allocator::deallocate(ptr);
}

void operator delete[](void *ptr) noexcept
{
    vigil::allocator::deallocate(ptr);
}

void operator delete(void *ptr, const std::nothrow_t &) noexcept
{
    vigil::allocator::deallocate(ptr);
}

void operator delete[](void *ptr, const std::nothrow_t &) noexcept
{
    vigil::allocator::deallocate(ptr);
}

#if defined(__cpp_sized_deallocation) && __cpp_sized_deallocation >= 201309L
void operator delete(void *ptr, std::size_t size) noexcept
{
    vigil::allocator::deallocate_sized(ptr, size);
}

void operator delete[](void *ptr, std::size_t size) noexcept
{
    vigil::allocator::deallocate_sized(ptr, size);
}
#endif

#if defined(__cpp_aligned_new) && __cpp_aligned_new >= 201606L
void *operator new(std::size_t size, std::align_val_t alignment)
{
    return vigil::allocator::allocate_aligned(size, static_cast<std::size_t>(alignment));
}

void *operator new[](std::size_t size, std::align_val_t alignment)
{
    return vigil::allocator::allocate_aligned(size, static_cast<std::size_t>(alignment));
}

void *operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t &) noexcept
{
    return vigil::allocator::allocate_aligned_nothrow(size, static_cast<std::size_t>(alignment));
}

void *operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t &) noexcept
{
    return vigil::allocator::allocate_aligned_nothrow(size, static_cast<std::size_t>(alignment));
}

void operator delete(void *ptr, std::align_val_t) noexcept
{
    vigil::allocator::deallocate(ptr);
}

void operator delete[](void *ptr, std::align_val_t) noexcept
{
    vigil::allocator::deallocate(ptr);
}

void operator delete(void *ptr, std::align_val_t, const std::nothrow_t &) noexcept
{
    vigil::allocator::deallocate(ptr);
}

void operator delete[](void *ptr, std::align_val_t, const std::nothrow_t &) noexcept
{
    vigil::allocator::deallocate(ptr);
}

void operator delete(void *ptr, std::size_t size, std::align_val_t alignment) noexcept
{
    vigil::allocator::deallocate_sized_aligned(ptr, size, static_cast<std::size_t>(alignment));
}

void operator delete[](void *ptr, std::size_t size, std::align_val_t alignment) noexcept
{
    vigil::allocator::deallocate_sized_aligned(ptr, size, static_cast<std::size_t>(alignment));
}
#endif
