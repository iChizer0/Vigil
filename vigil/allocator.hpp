#pragma once
#ifndef VIGIL_ALLOCATOR_HPP
#define VIGIL_ALLOCATOR_HPP

#include <cstddef>
#include <new>

#include <jemalloc/jemalloc.h>

#if !defined(__GNUC__) && !defined(__clang__)
#error "Vigil jemalloc allocator currently supports GCC/Clang-compatible compilers only."
#endif

#if !defined(VIGIL_ALLOCATOR_ALWAYS_INLINE)
#define VIGIL_ALLOCATOR_ALWAYS_INLINE inline __attribute__((always_inline))
#endif

#if !defined(VIGIL_ALLOCATOR_NOINLINE)
#define VIGIL_ALLOCATOR_NOINLINE __attribute__((noinline))
#endif

#if !defined(VIGIL_ALLOCATOR_COLD)
#define VIGIL_ALLOCATOR_COLD __attribute__((cold))
#endif

#if !defined(VIGIL_ALLOCATOR_LIKELY)
#define VIGIL_ALLOCATOR_LIKELY(expr) __builtin_expect(!!(expr), 1)
#endif

#if !defined(VIGIL_ALLOCATOR_UNLIKELY)
#define VIGIL_ALLOCATOR_UNLIKELY(expr) __builtin_expect(!!(expr), 0)
#endif

namespace vigil::allocator {

namespace detail {

    [[nodiscard]] static constexpr std::size_t normalize_size(std::size_t size) noexcept
    {
        return size == 0 ? 1 : size;
    }

    [[nodiscard]] static constexpr bool is_power_of_two(std::size_t value) noexcept
    {
        return value != 0 && (value & (value - 1)) == 0;
    }

    [[nodiscard]] VIGIL_ALLOCATOR_ALWAYS_INLINE void *try_allocate(std::size_t size) noexcept
    {
        return je_malloc(normalize_size(size));
    }

    [[nodiscard]] VIGIL_ALLOCATOR_ALWAYS_INLINE void *try_allocate_aligned(std::size_t size,
        std::size_t alignment) noexcept
    {
        if (VIGIL_ALLOCATOR_LIKELY(alignment <= alignof(std::max_align_t)))
            return try_allocate(size);
        if (VIGIL_ALLOCATOR_UNLIKELY(!is_power_of_two(alignment)))
            return nullptr;
        return je_mallocx(normalize_size(size), MALLOCX_ALIGN(alignment));
    }

    template<typename AllocateFn>
    [[nodiscard]] VIGIL_ALLOCATOR_NOINLINE VIGIL_ALLOCATOR_COLD void *allocate_with_new_handler(
        AllocateFn &&allocate_fn)
    {
        for (;;)
        {
            if (void *ptr = allocate_fn())
                return ptr;

            const std::new_handler handler = std::get_new_handler();
            if (VIGIL_ALLOCATOR_UNLIKELY(!handler))
                throw std::bad_alloc();
            handler();
        }
    }

    template<typename AllocateFn>
    [[nodiscard]] VIGIL_ALLOCATOR_NOINLINE VIGIL_ALLOCATOR_COLD void *allocate_with_new_handler_nothrow(
        AllocateFn &&allocate_fn) noexcept
    {
        for (;;)
        {
            if (void *ptr = allocate_fn())
                return ptr;

            const std::new_handler handler = std::get_new_handler();
            if (VIGIL_ALLOCATOR_UNLIKELY(!handler))
                return nullptr;
            try
            {
                handler();
            }
            catch (...)
            {
                return nullptr;
            }
        }
    }

} // namespace detail

[[nodiscard]] VIGIL_ALLOCATOR_ALWAYS_INLINE bool using_jemalloc() noexcept
{
    return true;
}

[[nodiscard]] VIGIL_ALLOCATOR_ALWAYS_INLINE void *allocate(std::size_t size)
{
    if (void *ptr = detail::try_allocate(size); VIGIL_ALLOCATOR_LIKELY(ptr != nullptr))
        return ptr;
    return detail::allocate_with_new_handler([size]() noexcept { return detail::try_allocate(size); });
}

[[nodiscard]] VIGIL_ALLOCATOR_ALWAYS_INLINE void *allocate_aligned(std::size_t size, std::size_t alignment)
{
    if (VIGIL_ALLOCATOR_LIKELY(alignment <= alignof(std::max_align_t)))
        return allocate(size);
    if (VIGIL_ALLOCATOR_UNLIKELY(!detail::is_power_of_two(alignment)))
        throw std::bad_alloc();
    if (void *ptr = detail::try_allocate_aligned(size, alignment); VIGIL_ALLOCATOR_LIKELY(ptr != nullptr))
        return ptr;
    return detail::allocate_with_new_handler(
        [size, alignment]() noexcept { return detail::try_allocate_aligned(size, alignment); });
}

[[nodiscard]] VIGIL_ALLOCATOR_ALWAYS_INLINE void *allocate_nothrow(std::size_t size) noexcept
{
    if (void *ptr = detail::try_allocate(size); VIGIL_ALLOCATOR_LIKELY(ptr != nullptr))
        return ptr;
    return detail::allocate_with_new_handler_nothrow([size]() noexcept { return detail::try_allocate(size); });
}

[[nodiscard]] VIGIL_ALLOCATOR_ALWAYS_INLINE void *allocate_aligned_nothrow(std::size_t size,
    std::size_t alignment) noexcept
{
    if (VIGIL_ALLOCATOR_LIKELY(alignment <= alignof(std::max_align_t)))
        return allocate_nothrow(size);
    if (VIGIL_ALLOCATOR_UNLIKELY(!detail::is_power_of_two(alignment)))
        return nullptr;
    if (void *ptr = detail::try_allocate_aligned(size, alignment); VIGIL_ALLOCATOR_LIKELY(ptr != nullptr))
        return ptr;
    return detail::allocate_with_new_handler_nothrow(
        [size, alignment]() noexcept { return detail::try_allocate_aligned(size, alignment); });
}

VIGIL_ALLOCATOR_ALWAYS_INLINE void deallocate(void *ptr) noexcept
{
    je_free(ptr);
}

VIGIL_ALLOCATOR_ALWAYS_INLINE void deallocate_sized(void *ptr, std::size_t size) noexcept
{
    if (VIGIL_ALLOCATOR_UNLIKELY(ptr == nullptr))
        return;
    je_sdallocx(ptr, detail::normalize_size(size), 0);
}

VIGIL_ALLOCATOR_ALWAYS_INLINE void deallocate_sized_aligned(void *ptr, std::size_t size, std::size_t alignment) noexcept
{
    if (VIGIL_ALLOCATOR_UNLIKELY(ptr == nullptr))
        return;
    if (VIGIL_ALLOCATOR_LIKELY(alignment <= alignof(std::max_align_t)))
    {
        deallocate_sized(ptr, size);
        return;
    }
    if (VIGIL_ALLOCATOR_UNLIKELY(!detail::is_power_of_two(alignment)))
    {
        deallocate(ptr);
        return;
    }
    je_sdallocx(ptr, detail::normalize_size(size), MALLOCX_ALIGN(alignment));
}

} // namespace vigil::allocator

#endif
