// Copyright 2016-2017 Francesco Biscani (bluescarni@gmail.com)
//
// This file is part of the mp++ library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef MPPP_INTEGER_HPP
#define MPPP_INTEGER_HPP

#include <mp++/config.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <string>
#if MPPP_CPLUSPLUS >= 201703L
#include <string_view>
#endif
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

#include <mp++/concepts.hpp>
#include <mp++/detail/fwd_decl.hpp>
#include <mp++/detail/gmp.hpp>
#if defined(MPPP_WITH_MPFR)
#include <mp++/detail/mpfr.hpp>
#endif
#include <mp++/detail/type_traits.hpp>
#include <mp++/detail/utils.hpp>
#include <mp++/exceptions.hpp>

// Compiler configuration.
// NOTE: check for MSVC first, as clang-cl does define both __clang__ and _MSC_VER,
// and we want to configure it as MSVC.
#if defined(_MSC_VER)

// Disable clang-format here, as the include order seems to matter.
// clang-format off
#include <windows.h>
#include <Winnt.h>
// clang-format on

// Disable some warnings for MSVC.
#pragma warning(push)
#pragma warning(disable : 4127)
#pragma warning(disable : 4146)
#pragma warning(disable : 4804)

// Checked iterators functionality.
#include <iterator>

#elif defined(__clang__) || defined(__GNUC__) || defined(__INTEL_COMPILER)

// NOTE: we can check int128 on GCC/clang with __SIZEOF_INT128__ apparently:
// http://stackoverflow.com/questions/21886985/what-gcc-versions-support-the-int128-intrinsic-type
#if defined(__SIZEOF_INT128__)
#define MPPP_UINT128 __uint128_t
#endif

#endif

namespace mppp
{

inline namespace detail
{
// Some misc tests to check that the mpz struct conforms to our expectations.
// This is crucial for the implementation of the union integer type.
struct expected_mpz_struct_t {
    mpz_alloc_t _mp_alloc;
    mpz_size_t _mp_size;
    ::mp_limb_t *_mp_d;
};

static_assert(sizeof(expected_mpz_struct_t) == sizeof(mpz_struct_t) && std::is_standard_layout<mpz_struct_t>::value
                  && std::is_standard_layout<expected_mpz_struct_t>::value && offsetof(mpz_struct_t, _mp_alloc) == 0u
                  && offsetof(mpz_struct_t, _mp_size) == offsetof(expected_mpz_struct_t, _mp_size)
                  && offsetof(mpz_struct_t, _mp_d) == offsetof(expected_mpz_struct_t, _mp_d)
                  && std::is_same<::mp_limb_t *, decltype(std::declval<mpz_struct_t>()._mp_d)>::value &&
                  // mp_bitcnt_t is used in shift operators, so we double-check it is unsigned. If it is not
                  // we might end up shifting by negative values, which is UB.
                  std::is_unsigned<::mp_bitcnt_t>::value &&
                  // The mpn functions accept sizes as ::mp_size_t, but we generally represent sizes as mpz_size_t.
                  // We need then to make sure we can always cast safely mpz_size_t to ::mp_size_t. Inspection
                  // of the gmp.h header seems to indicate that mpz_size_t is never larger than ::mp_size_t.
                  std::numeric_limits<mpz_size_t>::min() >= std::numeric_limits<::mp_size_t>::min()
                  && std::numeric_limits<mpz_size_t>::max() <= std::numeric_limits<::mp_size_t>::max(),
              "Invalid mpz_t struct layout and/or GMP types.");

#if MPPP_CPLUSPLUS >= 201703L

// If we have C++17, we can use structured bindings to test the layout of mpz_struct_t
// and its members' types.
constexpr bool test_mpz_struct_t()
{
    // NOTE: if mpz_struct_t has more or fewer members, this will result
    // in a compile-time error.
    auto[alloc, size, ptr] = mpz_struct_t{};
    (void)alloc;
    (void)size;
    (void)ptr;
    return std::is_same<decltype(alloc), mpz_alloc_t>::value && std::is_same<decltype(size), mpz_size_t>::value
           && std::is_same<decltype(ptr), ::mp_limb_t *>::value;
}

static_assert(test_mpz_struct_t(), "The mpz_struct_t does not have the expected layout.");

#endif

// The reason we are asserting this is the following: in a few places we are using the wrap-around property
// of unsigned arithmetics, but if mp_limb_t is a narrow unsigned type (e.g., unsigned short or unsigned char)
// then there could be a promotion to other types triggered by the standard integral promotions,
// and the wrap around behaviour would not be there any more. This is just a theoretical concern at the moment.
static_assert(disjunction<std::is_same<::mp_limb_t, unsigned long>, std::is_same<::mp_limb_t, unsigned long long>,
                          std::is_same<::mp_limb_t, unsigned>>::value,
              "Invalid type for mp_limb_t.");

// Small helper to get the size in limbs from an mpz_t. Will return zero if n is zero.
inline std::size_t get_mpz_size(const ::mpz_t n)
{
    return (n->_mp_size >= 0) ? static_cast<std::size_t>(n->_mp_size) : static_cast<std::size_t>(nint_abs(n->_mp_size));
}

// Structure for caching allocated arrays of limbs.
struct mpz_alloc_cache {
    // Arrays up to this size will be cached.
    static constexpr std::size_t max_size = 10;
    // Max number of arrays to cache for each size.
    static constexpr std::size_t max_entries = 100;
    // The actual cache.
    std::array<std::array<::mp_limb_t *, max_entries>, max_size> caches;
    // The number of arrays actually stored in each cache entry.
    std::array<std::size_t, max_size> sizes;
    // NOTE: use round brackets init for the usual GCC 4.8 workaround.
    constexpr mpz_alloc_cache() : caches(), sizes() {}
    ~mpz_alloc_cache()
    {
#if !defined(NDEBUG)
        std::cout << "Cleaning up the mpz alloc cache." << std::endl;
#endif
        void (*ffp)(void *, std::size_t);
        ::mp_get_memory_functions(nullptr, nullptr, &ffp);
        for (std::size_t i = 0; i < max_size; ++i) {
            for (std::size_t j = 0; j < sizes[i]; ++j) {
                ffp(static_cast<void *>(caches[i][j]), i + 1u);
            }
        }
    }
};

#if defined(MPPP_HAVE_THREAD_LOCAL)

template <typename = void>
struct mpz_caches {
    static thread_local mpz_alloc_cache a_cache;
};

template <typename T>
thread_local mpz_alloc_cache mpz_caches<T>::a_cache;

#endif

// Helper function to init an mpz to zero with nlimbs preallocated limbs.
inline void mpz_init_nlimbs(mpz_struct_t &rop, std::size_t nlimbs)
{
#if defined(MPPP_HAVE_THREAD_LOCAL)
    auto &mpzc = mpz_caches<>::a_cache;
    if (nlimbs && nlimbs <= mpzc.max_size && mpzc.sizes[nlimbs - 1u]) {
        // LCOV_EXCL_START
        if (mppp_unlikely(nlimbs > static_cast<make_unsigned_t<mpz_size_t>>(std::numeric_limits<mpz_size_t>::max()))) {
            std::abort();
        }
        // LCOV_EXCL_STOP
        const auto idx = nlimbs - 1u;
        rop._mp_alloc = static_cast<mpz_size_t>(nlimbs);
        rop._mp_size = 0;
        rop._mp_d = mpzc.caches[idx][mpzc.sizes[idx] - 1u];
        --mpzc.sizes[idx];
    } else {
#endif
        // LCOV_EXCL_START
        // A bit of horrid overflow checking.
        if (mppp_unlikely(nlimbs > std::numeric_limits<std::size_t>::max() / unsigned(GMP_NUMB_BITS))) {
            // NOTE: here we are doing what GMP does in case of memory allocation errors. It does not make much sense
            // to do anything else, as long as GMP does not provide error recovery.
            std::abort();
        }
        // LCOV_EXCL_STOP
        const auto nbits = static_cast<std::size_t>(unsigned(GMP_NUMB_BITS) * nlimbs);
        // LCOV_EXCL_START
        if (mppp_unlikely(nbits > std::numeric_limits<::mp_bitcnt_t>::max())) {
            std::abort();
        }
        // LCOV_EXCL_STOP
        // NOTE: nbits == 0 is allowed.
        ::mpz_init2(&rop, static_cast<::mp_bitcnt_t>(nbits));
        assert(make_unsigned_t<mpz_size_t>(rop._mp_alloc) >= nlimbs);
#if defined(MPPP_HAVE_THREAD_LOCAL)
    }
#endif
}

// Thin wrapper around mpz_clear(): will add entry to cache if possible instead of clearing.
inline void mpz_clear_wrap(mpz_struct_t &m)
{
#if defined(MPPP_HAVE_THREAD_LOCAL)
    auto &mpzc = mpz_caches<>::a_cache;
    const auto ualloc = static_cast<make_unsigned_t<mpz_size_t>>(m._mp_alloc);
    if (ualloc && ualloc <= mpzc.max_size && mpzc.sizes[ualloc - 1u] < mpzc.max_entries) {
        const auto idx = ualloc - 1u;
        mpzc.caches[idx][mpzc.sizes[idx]] = m._mp_d;
        ++mpzc.sizes[idx];
    } else {
#endif
        ::mpz_clear(&m);
#if defined(MPPP_HAVE_THREAD_LOCAL)
    }
#endif
}

// Combined init+set.
inline void mpz_init_set_nlimbs(mpz_struct_t &m0, const mpz_struct_t &m1)
{
    mpz_init_nlimbs(m0, get_mpz_size(&m1));
    ::mpz_set(&m0, &m1);
}

// Convert an mpz to a string in a specific base, to be written into out.
inline void mpz_to_str(std::vector<char> &out, const mpz_struct_t *mpz, int base = 10)
{
    assert(base >= 2 && base <= 62);
    const auto size_base = ::mpz_sizeinbase(mpz, base);
    // LCOV_EXCL_START
    if (mppp_unlikely(size_base > std::numeric_limits<std::size_t>::max() - 2u)) {
        throw std::overflow_error("Too many digits in the conversion of mpz_t to string.");
    }
    // LCOV_EXCL_STOP
    // Total max size is the size in base plus an optional sign and the null terminator.
    const auto total_size = size_base + 2u;
    // NOTE: possible improvement: use a null allocator to avoid initing the chars each time
    // we resize up.
    // Overflow check.
    // LCOV_EXCL_START
    if (mppp_unlikely(total_size > std::numeric_limits<std::vector<char>::size_type>::max())) {
        throw std::overflow_error("Too many digits in the conversion of mpz_t to string.");
    }
    // LCOV_EXCL_STOP
    out.resize(static_cast<std::vector<char>::size_type>(total_size));
    ::mpz_get_str(out.data(), base, mpz);
}

// Convenience overload for the above.
inline std::string mpz_to_str(const mpz_struct_t *mpz, int base = 10)
{
    MPPP_MAYBE_TLS std::vector<char> tmp;
    mpz_to_str(tmp, mpz, base);
    return tmp.data();
}

// Small wrapper to copy limbs.
inline void copy_limbs(const ::mp_limb_t *begin, const ::mp_limb_t *end, ::mp_limb_t *out)
{
    for (; begin != end; ++begin, ++out) {
        *out = *begin;
    }
}

// The version with non-overlapping ranges.
inline void copy_limbs_no(const ::mp_limb_t *begin, const ::mp_limb_t *end, ::mp_limb_t *MPPP_RESTRICT out)
{
    assert(begin != out);
    for (; begin != end; ++begin, ++out) {
        *out = *begin;
    }
}

// Add a and b, store the result in res, and return 1 if there's unsigned overflow, 0 otherwise.
// NOTE: recent GCC versions have builtins for this, but they don't seem to make much of a difference:
// https://gcc.gnu.org/onlinedocs/gcc/Integer-Overflow-Builtins.html
inline ::mp_limb_t limb_add_overflow(::mp_limb_t a, ::mp_limb_t b, ::mp_limb_t *res)
{
    *res = a + b;
    return *res < a;
}

// Implementation of the function to count the number of leading zeroes
// in an unsigned integral value. Only GCC and clang for now.
// NOTE: MSVC seems to have similar intrinsics, but it's not clear if they
// are available only on certain platforms:
// https://msdn.microsoft.com/en-us/library/bb384809.aspx
// https://stackoverflow.com/questions/355967/how-to-use-msvc-intrinsics-to-get-the-equivalent-of-this-gcc-code/20468180
// Let's implement it only on clang/GCC for now.
#if defined(__clang__) || defined(__GNUC__)

// Dispatch based on the integer type.
inline int builtin_clz_impl(unsigned n)
{
    return __builtin_clz(n);
}

inline int builtin_clz_impl(unsigned long n)
{
    return __builtin_clzl(n);
}

inline int builtin_clz_impl(unsigned long long n)
{
    return __builtin_clzll(n);
}

// NOTE: we checked earlier that mp_limb_t is one of the 3 types supported by the
// clz builtin. No need to constrain the template.
template <typename T>
inline unsigned builtin_clz(T n)
{
    assert(n != 0u);
    return static_cast<unsigned>(builtin_clz_impl(n));
}

#endif

// The static integer class.
template <std::size_t SSize>
struct static_int {
    // Let's put a hard cap and sanity check on the static size.
    static_assert(SSize > 0u && SSize <= 64u, "Invalid static size for integer.");
    using limbs_type = std::array<::mp_limb_t, SSize>;
    // Cast it to mpz_size_t for convenience.
    static const mpz_size_t s_size = SSize;
    // Special alloc value to signal static storage in the union.
    static const mpz_alloc_t s_alloc = -1;
    // The largest number of limbs for which special optimisations are activated.
    // This is important because the arithmetic optimisations rely on the unused limbs
    // being zero, thus whenever we use mpn functions on a static int we need to
    // take care of ensuring that this invariant is respected (see dtor_checks() and
    // zero_unused_limbs(), for instance).
    static const std::size_t opt_size = 2;
    // NOTE: init limbs to zero, in order to avoid reading uninited limbs during copies/moves
    // (additionally, in some few-limbs optimisations we operate on the whole limb
    // array regardless of the integer size, for performance reasons - if we didn't init to zero,
    // we would have wrong results as well).
    // NOTE: it might be possible here to avoid the zero init of the limbs, at least in case
    // of static sizes > opt_size, but it's not clear to me if it is worth it to go down this path.
    // Let's just mention it for now. Note that the matter of zeroing the unused limbs is not
    // always dealt with consistenly throughout the code: here and elsewhere (e.g., set_zero(), and see
    // also some equivalent bits in rational) we zero regardless, but in zero_unused_limbs() we check
    // about opt_size. Let's leave this discussion for when we optimize the mpn_ implementations
    // (if ever).
    static_int() : _mp_size(0), m_limbs() {}
    // Let's avoid copying the _mp_alloc member, as it is never written to and it must always
    // have the same value.
    static_int(const static_int &other) : _mp_size(other._mp_size), m_limbs(other.m_limbs) {}
    static_int(static_int &&other) noexcept : static_int(other) {}
    // These 2 constructors are used in the generic constructor of integer_union.
    //
    // Constructor from a size and a single limb (will be the least significant limb).
    explicit static_int(mpz_size_t size, ::mp_limb_t l) : _mp_size(size)
    {
        // Input sanity checks.
        assert(size <= s_size && size >= -s_size);
        m_limbs[0] = l;
        // Zero fill the remaining limbs.
        std::fill(m_limbs.begin() + 1, m_limbs.end(), ::mp_limb_t(0));
    }
    // Constructor from a (signed) size and a limb range. The limbs in the range will be
    // copied as the least significant limbs.
    explicit static_int(mpz_size_t size, const ::mp_limb_t *begin, std::size_t asize) : _mp_size(size)
    {
        // Input sanity checks.
        assert(asize <= SSize);
        assert(size <= s_size && size >= -s_size);
        assert(size == static_cast<mpz_size_t>(asize) || size == -static_cast<mpz_size_t>(asize));
        // Copy the input limbs.
        // NOTE: here we are constructing a *new* object, thus I don't think it's possible that begin
        // overlaps with m_limbs.data() (unless some UBish stuff is being attempted).
        copy_limbs_no(begin, begin + asize, m_limbs.data());
        // Zero the remaining limbs, if any.
        std::fill(m_limbs.begin() + asize, m_limbs.end(), ::mp_limb_t(0));
    }
    static_int &operator=(const static_int &other)
    {
        _mp_size = other._mp_size;
        // NOTE: self assignment of std::array should be fine.
        m_limbs = other.m_limbs;
        return *this;
    }
    static_int &operator=(static_int &&other) noexcept
    {
        // Just forward to the copy assignment.
        return operator=(other);
    }
    bool dtor_checks() const
    {
        // LCOV_EXCL_START
        const auto asize = abs_size();
        // Check the value of the alloc member.
        if (_mp_alloc != s_alloc) {
            return false;
        }
        // Check the asize is not too large.
        if (asize > s_size) {
            return false;
        }
        // Check that all limbs which do not participate in the value are zero, iff the SSize is small enough.
        // For small SSize, we might be using optimisations that require unused limbs to be zero.
        if (SSize <= opt_size) {
            for (auto i = static_cast<std::size_t>(asize); i < SSize; ++i) {
                if (m_limbs[i]) {
                    return false;
                }
            }
        }
        // Check that the highest limb is not zero.
        if (asize > 0 && (m_limbs[static_cast<std::size_t>(asize - 1)] & GMP_NUMB_MASK) == 0u) {
            return false;
        }
        return true;
        // LCOV_EXCL_STOP
    }
    ~static_int()
    {
        assert(dtor_checks());
    }
    // Zero the limbs that are not used for representing the value.
    // This is normally not needed, but it is useful when using the GMP mpn api on a static int:
    // the GMP api does not clear unused limbs, but we rely on unused limbs being zero when optimizing operations
    // for few static limbs.
    void zero_unused_limbs()
    {
        // Don't do anything if the static size is larger that the maximum size for which the
        // few-limbs optimisations are activated.
        if (SSize <= opt_size) {
            for (auto i = static_cast<std::size_t>(abs_size()); i < SSize; ++i) {
                m_limbs[i] = 0u;
            }
        }
    }
    // Size in limbs (absolute value of the _mp_size member).
    mpz_size_t abs_size() const
    {
        return _mp_size >= 0 ? _mp_size : -_mp_size;
    }
    // NOTE: the retval here can be used only in read-only mode, otherwise
    // we will have UB due to the const_cast use.
    mpz_struct_t get_mpz_view() const
    {
        return {_mp_alloc, _mp_size, const_cast<::mp_limb_t *>(m_limbs.data())};
    }
    mpz_alloc_t _mp_alloc = s_alloc;
    mpz_size_t _mp_size;
    limbs_type m_limbs;
};

// {static_int,mpz} union.
template <std::size_t SSize>
union integer_union {
    using s_storage = static_int<SSize>;
    using d_storage = mpz_struct_t;
    // Def ctor, will init to static.
    integer_union() : m_st() {}
    // Copy constructor, does a deep copy maintaining the storage class of other.
    integer_union(const integer_union &other)
    {
        if (other.is_static()) {
            ::new (static_cast<void *>(&m_st)) s_storage(other.g_st());
        } else {
            ::new (static_cast<void *>(&m_dy)) d_storage;
            mpz_init_set_nlimbs(m_dy, other.g_dy());
            assert(m_dy._mp_alloc >= 0);
        }
    }
    // Move constructor. Will downgrade other to a static zero integer if other is dynamic.
    integer_union(integer_union &&other) noexcept
    {
        if (other.is_static()) {
            // Activate the static member with a copy.
            ::new (static_cast<void *>(&m_st)) s_storage(other.g_st());
        } else {
            // Activate dynamic member and shallow copy it from other.
            ::new (static_cast<void *>(&m_dy)) d_storage(other.g_dy());
            // Downgrade the other to an empty static.
            other.g_dy().~d_storage();
            ::new (static_cast<void *>(&other.m_st)) s_storage();
        }
    }
    // Machinery for the implementation of the generic ctor follows.
    // NOTE: the idea of having the generic ctors and the ctors from string implemented in the union
    // is that it allows the direct construction of the integer object. If the constructors' logic
    // were implemented in the integer class, we would have first to def-init the union and then
    // modify it. Like this, we directly initialise just once the data.
    //
    // Add a limb at the top of the integer (that is, the new limb will be the new most
    // significant limb). Requires a non-negative integer. This is used only during generic construction,
    // do **NOT** use it anywhere else.
    void push_limb(::mp_limb_t l)
    {
        const auto asize = m_st._mp_size;
        assert(asize >= 0);
        if (is_static()) {
            if (std::size_t(asize) < SSize) {
                // If there's space for an extra limb, write it, update the size,
                // and return.
                ++g_st()._mp_size;
                g_st().m_limbs[std::size_t(asize)] = l;
                return;
            }
            // Store the upper limb.
            const auto limb_copy = g_st().m_limbs[SSize - 1u];
            // Make sure the upper limb contains something.
            // NOTE: This is necessary because this method is used while building an integer during generic
            // construction, and it might be possible that the current top limb is zero. If that is the case,
            // the promotion below triggers an assertion failure when destroying
            // the static int in debug mode.
            g_st().m_limbs[SSize - 1u] = 1u;
            // There's no space for the extra limb. Promote the integer and move on.
            promote(SSize + 1u);
            // Recover the upper limb.
            g_dy()._mp_d[SSize - 1u] = limb_copy;
        }
        // LCOV_EXCL_START
        if (g_dy()._mp_alloc == asize) {
            // There is not enough space for the extra limb, we need to reallocate.
            // NOTE: same as above, make sure the top limb contains something. mpz_realloc2() seems not to care,
            // but better safe than sorry.
            // NOTE: in practice this is never hit on current architectures: the only case we end up here is initing
            // with a 64bit integer when the limb is 32bit, but in that case we already promoted to size 2 above.
            const auto limb_copy = g_dy()._mp_d[asize - 1];
            g_dy()._mp_d[asize - 1] = 1u;
            // NOTE: we check at the top of dispatch_generic_ctor() that we can compute the second function
            // argument safely.
            ::mpz_realloc2(&g_dy(), static_cast<::mp_bitcnt_t>(asize) * 2u * unsigned(GMP_NUMB_BITS));
            g_dy()._mp_d[asize - 1] = limb_copy;
        }
        // LCOV_EXCL_STOP
        // Write the extra limb, update the size.
        ++g_dy()._mp_size;
        g_dy()._mp_d[asize] = l;
    }
    // This is a small utility function to shift down the unsigned integer n by GMP_NUMB_BITS.
    // If GMP_NUMB_BITS is not smaller than the bit size of T, then an assertion will fire. We need this
    // little helper in order to avoid compiler warnings.
    template <typename T, enable_if_t<(GMP_NUMB_BITS < std::numeric_limits<T>::digits), int> = 0>
    static void checked_rshift(T &n)
    {
        n >>= GMP_NUMB_BITS;
    }
    template <typename T, enable_if_t<(GMP_NUMB_BITS >= std::numeric_limits<T>::digits), int> = 0>
    static void checked_rshift(T &)
    {
        assert(false);
    }
    // Special casing for bool.
    void dispatch_generic_ctor(bool b)
    {
        // Construct the static. No need for masking in the only limb.
        ::new (static_cast<void *>(&m_st)) s_storage{static_cast<mpz_size_t>(b), static_cast<::mp_limb_t>(b)};
    }
    // Construction from unsigned ints. The Neg flag will negate the integer after construction, it is for use in
    // the constructor from signed ints.
    template <typename T, bool Neg = false,
              enable_if_t<conjunction<std::is_integral<T>, std::is_unsigned<T>>::value, int> = 0>
    void dispatch_generic_ctor(T n)
    {
        // NOTE: in push_limb() we need to compute a bit size for mpz_realloc2(), which might be up to twice
        // the bit size of T. Make sure we can do the computation safely.
        static_assert(unsigned(std::numeric_limits<T>::digits) <= std::numeric_limits<::mp_bitcnt_t>::max() / 2u,
                      "Invalid bit width for an unsigned integral type.");
        if (n <= GMP_NUMB_MAX) {
            // Special codepath if n fits directly in a single limb.
            // No need for the mask as we are sure that n <= GMP_NUMB_MAX.
            ::new (static_cast<void *>(&m_st)) s_storage{Neg ? -(n != 0u) : (n != 0u), static_cast<::mp_limb_t>(n)};
            return;
        }
        // Def construct a static before building a multi-limb integer.
        ::new (static_cast<void *>(&m_st)) s_storage();
        while (n) {
            push_limb(static_cast<::mp_limb_t>(n & GMP_NUMB_MASK));
            if (GMP_NUMB_BITS >= std::numeric_limits<T>::digits) {
                break;
            }
            checked_rshift(n);
        }
        // Negate if requested.
        if (Neg) {
            neg();
        }
    }
    // Construction from signed ints.
    template <typename T, enable_if_t<conjunction<std::is_integral<T>, std::is_signed<T>>::value, int> = 0>
    void dispatch_generic_ctor(T n)
    {
        if (n >= T(0)) {
            // Positive value, just cast to unsigned.
            dispatch_generic_ctor(static_cast<make_unsigned_t<T>>(n));
        } else {
            // Negative value, use its abs.
            dispatch_generic_ctor<make_unsigned_t<T>, true>(nint_abs(n));
        }
    }
    // Construction from float/double.
    template <typename T, enable_if_t<disjunction<std::is_same<T, float>, std::is_same<T, double>>::value, int> = 0>
    void dispatch_generic_ctor(T x)
    {
        if (mppp_unlikely(!std::isfinite(x))) {
            throw std::domain_error("Cannot construct an integer from the non-finite floating-point value "
                                    + std::to_string(x));
        }
        MPPP_MAYBE_TLS mpz_raii tmp;
        ::mpz_set_d(&tmp.m_mpz, static_cast<double>(x));
        dispatch_mpz_ctor(&tmp.m_mpz);
    }
#if defined(MPPP_WITH_MPFR)
    // Construction from long double, requires MPFR.
    template <typename T, enable_if_t<std::is_same<T, long double>::value, int> = 0>
    void dispatch_generic_ctor(T x)
    {
        if (mppp_unlikely(!std::isfinite(x))) {
            throw std::domain_error("Cannot construct an integer from the non-finite floating-point value "
                                    + std::to_string(x));
        }
        // NOTE: static checks for overflows are done in mpfr.hpp.
        constexpr int d2 = std::numeric_limits<long double>::max_digits10 * 4;
        MPPP_MAYBE_TLS mpfr_raii mpfr(static_cast<::mpfr_prec_t>(d2));
        MPPP_MAYBE_TLS mpz_raii tmp;
        ::mpfr_set_ld(&mpfr.m_mpfr, x, MPFR_RNDN);
        ::mpfr_get_z(&tmp.m_mpz, &mpfr.m_mpfr, MPFR_RNDZ);
        dispatch_mpz_ctor(&tmp.m_mpz);
    }
#endif
    // The generic constructor.
    template <typename T>
    explicit integer_union(const T &x)
    {
        dispatch_generic_ctor(x);
    }
    // Implementation of the constructor from string. Abstracted into separate function because it is re-used.
    void dispatch_c_string_ctor(const char *s, int base)
    {
        if (mppp_unlikely(base != 0 && (base < 2 || base > 62))) {
            throw std::invalid_argument(
                "In the constructor of integer from string, a base of " + std::to_string(base)
                + " was specified, but the only valid values are 0 and any value in the [2,62] range");
        }
        MPPP_MAYBE_TLS mpz_raii mpz;
        if (mppp_unlikely(::mpz_set_str(&mpz.m_mpz, s, base))) {
            if (base) {
                throw std::invalid_argument(std::string("The string '") + s + "' is not a valid integer in base "
                                            + std::to_string(base));
            } else {
                throw std::invalid_argument(std::string("The string '") + s
                                            + "' is not a valid integer in any supported base");
            }
        }
        dispatch_mpz_ctor(&mpz.m_mpz);
    }
    // Constructor from C string and base.
    explicit integer_union(const char *s, int base)
    {
        dispatch_c_string_ctor(s, base);
    }
    // Constructor from string range and base.
    explicit integer_union(const char *begin, const char *end, int base)
    {
        // Copy the range into a local buffer.
        MPPP_MAYBE_TLS std::vector<char> buffer;
        buffer.assign(begin, end);
        buffer.emplace_back('\0');
        dispatch_c_string_ctor(buffer.data(), base);
    }
    // Constructor from mpz_t. Abstracted into separate function because it is re-used.
    void dispatch_mpz_ctor(const ::mpz_t n)
    {
        const auto asize = get_mpz_size(n);
        if (asize > SSize) {
            // Too big, need to use dynamic.
            ::new (static_cast<void *>(&m_dy)) d_storage;
            mpz_init_set_nlimbs(m_dy, *n);
        } else {
            ::new (static_cast<void *>(&m_st)) s_storage{n->_mp_size, n->_mp_d, asize};
        }
    }
    explicit integer_union(const ::mpz_t n)
    {
        dispatch_mpz_ctor(n);
    }
#if !defined(_MSC_VER)
    // Move ctor from mpz_t.
    explicit integer_union(::mpz_t &&n)
    {
        const auto asize = get_mpz_size(n);
        if (asize > SSize) {
            // Too big, make shallow copy into dynamic. this will now
            // own the resources of n.
            ::new (static_cast<void *>(&m_dy)) d_storage(*n);
        } else {
            // Fits into small.
            ::new (static_cast<void *>(&m_st)) s_storage{n->_mp_size, n->_mp_d, asize};
            // Clear n: its resources have been copied into this, and we must
            // ensure uniform behaviour with the case in which we shallow-copied it
            // into dynamic storage.
            mpz_clear_wrap(*n);
        }
    }
#endif
    // Copy assignment operator, performs a deep copy maintaining the storage class.
    integer_union &operator=(const integer_union &other)
    {
        const bool s1 = is_static(), s2 = other.is_static();
        if (s1 && s2) {
            // Self assignment is fine, handled in the static.
            g_st() = other.g_st();
        } else if (s1 && !s2) {
            // Destroy static.
            g_st().~s_storage();
            // Construct the dynamic struct.
            ::new (static_cast<void *>(&m_dy)) d_storage;
            // Init + assign the mpz.
            mpz_init_set_nlimbs(m_dy, other.g_dy());
            assert(m_dy._mp_alloc >= 0);
        } else if (!s1 && s2) {
            // Destroy the dynamic this.
            destroy_dynamic();
            // Init-copy the static from other.
            ::new (static_cast<void *>(&m_st)) s_storage(other.g_st());
        } else {
            // Self assignment is fine, mpz_set() can have aliasing arguments.
            ::mpz_set(&g_dy(), &other.g_dy());
        }
        return *this;
    }
    // Move assignment, same as above plus possibly steals resources. If this is static
    // and other is dynamic, other is downgraded to a zero static.
    integer_union &operator=(integer_union &&other) noexcept
    {
        const bool s1 = is_static(), s2 = other.is_static();
        if (s1 && s2) {
            // Self assignment is fine, handled in the static.
            g_st() = other.g_st();
        } else if (s1 && !s2) {
            // Destroy static.
            g_st().~s_storage();
            // Construct the dynamic struct, shallow-copying from
            // other.
            ::new (static_cast<void *>(&m_dy)) d_storage(other.g_dy());
            // Downgrade the other to an empty static.
            other.g_dy().~d_storage();
            ::new (static_cast<void *>(&other.m_st)) s_storage();
        } else if (!s1 && s2) {
            // Same as copy assignment: destroy and copy-construct.
            destroy_dynamic();
            ::new (static_cast<void *>(&m_st)) s_storage(other.g_st());
        } else {
            // Swap with other. Self-assignment is fine, mpz_swap() can have
            // aliasing arguments.
            ::mpz_swap(&g_dy(), &other.g_dy());
        }
        return *this;
    }
    ~integer_union()
    {
        if (is_static()) {
            g_st().~s_storage();
        } else {
            destroy_dynamic();
        }
    }
    void destroy_dynamic()
    {
        assert(!is_static());
        assert(g_dy()._mp_alloc >= 0);
        assert(g_dy()._mp_d != nullptr);
        mpz_clear_wrap(g_dy());
        g_dy().~d_storage();
    }
    // Check storage type.
    bool is_static() const
    {
        return m_st._mp_alloc == s_storage::s_alloc;
    }
    bool is_dynamic() const
    {
        return m_st._mp_alloc != s_storage::s_alloc;
    }
    // Getters for st and dy.
    const s_storage &g_st() const
    {
        assert(is_static());
        return m_st;
    }
    s_storage &g_st()
    {
        assert(is_static());
        return m_st;
    }
    const d_storage &g_dy() const
    {
        assert(is_dynamic());
        return m_dy;
    }
    d_storage &g_dy()
    {
        assert(is_dynamic());
        return m_dy;
    }
    // Promotion from static to dynamic. If nlimbs != 0u, allocate nlimbs limbs, otherwise
    // allocate exactly the nlimbs necessary to represent this.
    void promote(std::size_t nlimbs = 0u)
    {
        assert(is_static());
        mpz_struct_t tmp_mpz;
        // Get a static_view.
        const auto v = g_st().get_mpz_view();
        if (nlimbs == 0u) {
            // If nlimbs is zero, we will allocate exactly the needed
            // number of limbs to represent this.
            mpz_init_set_nlimbs(tmp_mpz, v);
        } else {
            // Otherwise, we preallocate nlimbs and then set tmp_mpz
            // to the value of this.
            mpz_init_nlimbs(tmp_mpz, nlimbs);
            ::mpz_set(&tmp_mpz, &v);
        }
        // Destroy static.
        g_st().~s_storage();
        // Construct the dynamic struct.
        ::new (static_cast<void *>(&m_dy)) d_storage;
        m_dy = tmp_mpz;
    }
    // Demotion from dynamic to static.
    bool demote()
    {
        assert(is_dynamic());
        const auto dyn_size = get_mpz_size(&g_dy());
        // If the dynamic size is greater than the static size, we cannot demote.
        if (dyn_size > SSize) {
            return false;
        }
        // Copy over the limbs to temporary storage.
        std::array<::mp_limb_t, SSize> tmp;
        copy_limbs_no(g_dy()._mp_d, g_dy()._mp_d + dyn_size, tmp.data());
        const auto signed_size = g_dy()._mp_size;
        // Destroy the dynamic storage.
        destroy_dynamic();
        // Init the static storage with the saved data. The unused limbs will be zeroed
        // by the invoked static_int ctor.
        ::new (static_cast<void *>(&m_st)) s_storage{signed_size, tmp.data(), dyn_size};
        return true;
    }
    // Negation.
    void neg()
    {
        if (is_static()) {
            g_st()._mp_size = -g_st()._mp_size;
        } else {
            ::mpz_neg(&g_dy(), &g_dy());
        }
    }
    // NOTE: keep these public as we need them below.
    s_storage m_st;
    d_storage m_dy;
};
}

// Fwd declaration.
template <std::size_t SSize>
integer<SSize> &sqrt(integer<SSize> &, const integer<SSize> &);

// NOTE: a few misc things:
// - re-visit at one point the issue of the estimators when we need to promote from static to dynamic
//   in arithmetic ops. Currently they are not 100% optimal since they rely on the information coming out
//   of the static implementation rather than computing the estimated size of rop themselves, for performance
//   reasons (see comments);
// - maybe the lshift/rshift optimisation for 2 limbs could use dlimb types if available?
// - it seems like it might be possible to re-implement a bare-bone mpz api on top of the mpn primitives,
//   with the goal of providing some form of error recovery in case of memory errors (e.g., throw instead
//   of aborting). This is probably not an immediate concern though.
// - pow() can probably benefit for some specialised static implementation, especially in conjunction with
//   mpn_sqr().
// - gcd() can be improved (see notes).
// - functions still to be de-branched: all the mpn implementations, if worth it.
//   Probably better to wait for benchmarks before moving.
// - performance improvements for the assignment operators to integrals, at least (maybe floats as well?): avoid
//   cting temporary.
// - performance improvements for arithmetic with C++ integrals? (e.g., use add_ui() and similar rather than cting
//   temporary).

/// Multiprecision integer class.
/**
 * \rststar
 * This class represents arbitrary-precision signed integers. It acts as a wrapper around the GMP ``mpz_t`` type, with
 * a small value optimisation: integers whose size is up to ``SSize`` limbs are stored directly in the storage
 * occupied by the :cpp:class:`~mppp::integer` object, without resorting to dynamic memory allocation. The value of
 * ``SSize`` must be at least 1 and less than an implementation-defined upper limit. On most modern architectures,
 * a limb contains either 32 or 64 bits of data. Thus, for instance, is ``SSize`` is set to 2 on a 64-bit system,
 * the small value optimisation will be employed for all integral values less than :math:`2^{64 \times 2} = 2^{128}`.
 *
 * When the value of an :cpp:class:`~mppp::integer` is stored directly within the object, the *storage type* of the
 * integer is said to be *static*. When the limb size of the integer exceeds the maximum value ``SSize``, the storage
 * type becomes *dynamic*. The transition from static to dynamic storage happens transparently whenever the integer
 * value becomes large enough. The demotion from dynamic to static storage usually needs to be requested explicitly.
 * For values of ``SSize`` of 1 and 2, optimised implementations of basic arithmetic operations are employed,
 * if supported by the target architecture and if the storage type is static. For larger values of ``SSize``,
 * the ``mpn_`` low-level functions of the GMP API are used if the storage type is static. If the storage type is
 * dynamic, the usual ``mpz_`` functions from the GMP API are used.
 *
 * This class has the look and feel of a C++ builtin type: it can interact with most of C++'s integral and
 * floating-point primitive types (see the :cpp:concept:`~mppp::CppInteroperable` concept for the full list),
 * and it provides overloaded :ref:`operators <integer_operators>`. Differently from the builtin types,
 * however, this class does not allow any implicit conversion to/from other types (apart from ``bool``): construction
 * from and conversion to primitive types must always be requested explicitly. As a side effect, syntax such as
 *
 * .. code-block:: c++
 *
 *    integer<1> n = 5;
 *    int m = n;
 *
 * will not work, and direct initialization and explicit casting should be used instead:
 *
 * .. code-block:: c++
 *
 *    integer<1> n{5};
 *    int m = static_cast<int>(n);
 *
 * Most of the functionality is exposed via plain :ref:`functions <integer_functions>`, with the
 * general convention that the functions are named after the corresponding GMP functions minus the leading ``mpz_``
 * prefix. For instance, the GMP call
 *
 * .. code-block:: c++
 *
 *    mpz_add(rop,a,b);
 *
 * that writes the result of ``a + b`` into ``rop`` becomes simply
 *
 * .. code-block:: c++
 *
 *    add(rop,a,b);
 *
 * where the ``add()`` function is resolved via argument-dependent lookup. Function calls with overlapping arguments
 * are allowed, unless noted otherwise.
 *
 * Multiple overloads of the same functionality are often available.
 * Binary functions in GMP are usually implemented via three-arguments functions, in which the first
 * argument is a reference to the return value. The exponentiation function ``mpz_pow_ui()``, for instance,
 * takes three arguments: the return value, the base and the exponent. There are two overloads of the corresponding
 * :ref:`exponentiation <integer_exponentiation>` function:
 *
 * * a ternary overload semantically equivalent to ``mpz_pow_ui()``,
 * * a binary overload taking as inputs the base and the exponent, and returning the result
 *   of the exponentiation.
 *
 * This allows to avoid having to set up a return value for one-off invocations of ``pow_ui()`` (the binary overload
 * will do it for you). For example:
 *
 * .. code-block:: c++
 *
 *    integer<1> r1, r2, n{3};
 *    pow_ui(r1,n,2);   // Ternary pow_ui(): computes n**2 and stores
 *                      // the result in r1.
 *    r2 = pow_ui(n,2); // Binary pow_ui(): returns n**2, which is then
 *                      // assigned to r2.
 *
 * In case of unary functions, there are often three overloads available:
 *
 * * a binary overload taking as first parameter a reference to the return value (GMP style),
 * * a unary overload returning the result of the operation,
 * * a nullary member function that modifies the calling object in-place.
 *
 * For instance, here are three possible ways of computing the absolute value:
 *
 * .. code-block:: c++
 *
 *    integer<1> r1, r2, n{-5};
 *    abs(r1,n);   // Binary abs(): computes and stores the absolute value
 *                 // of n into r1.
 *    r2 = abs(n); // Unary abs(): returns the absolute value of n, which is
 *                 // then assigned to r2.
 *    n.abs();     // Member function abs(): replaces the value of n with its
 *                 // absolute value.
 *
 * Note that at this time only a small subset of the GMP API has been wrapped by :cpp:class:`~mppp::integer`.
 *
 * Various :ref:`overloaded operators <integer_operators>` are provided.
 * For the common arithmetic operations (``+``, ``-``, ``*`` and ``/``), the type promotion
 * rules are a natural extension of the corresponding rules for native C++ types: if the other argument
 * is a C++ integral, the result will be of type :cpp:class:`~mppp::integer`, if the other argument is a C++
 * floating-point the result will be of the same floating-point type. For example:
 *
 * .. code-block:: c++
 *
 *    integer<1> n1{1}, n2{2};
 *    auto res1 = n1 + n2; // res1 is an integer
 *    auto res2 = n1 * 2; // res2 is an integer
 *    auto res3 = 2 - n2; // res3 is an integer
 *    auto res4 = n1 / 2.f; // res4 is a float
 *    auto res5 = 12. / n1; // res5 is a double
 *
 * The modulo operator ``%`` accepts only :cpp:class:`~mppp::integer` and :cpp:concept:`~mppp::CppInteroperable`
 * integral types as arguments,
 * and it always returns :cpp:class:`~mppp::integer` as result. The bit shifting operators ``<<`` and ``>>`` accept
 * only :cpp:concept:`~mppp::CppInteroperable` integral types as shift arguments, and they always return
 * :cpp:class:`~mppp::integer` as result.
 *
 * The relational operators, ``==``, ``!=``, ``<``, ``>``, ``<=`` and ``>=`` will promote the arguments to a common type
 * before comparing them. The promotion rules are the same as in the arithmetic operators (that is, both arguments are
 * promoted to :cpp:class:`~mppp::integer` if they are both integral types, otherwise they are promoted to the type
 * of the floating-point argument).
 *
 * Several facilities for interfacing with the GMP library are provided. Specifically, :cpp:class:`~mppp::integer`
 * features:
 *
 * * a constructor and an assignment operator from the GMP integer type ``mpz_t``,
 * * a :cpp:func:`~mppp::integer::get_mpz_t()` method that promotes ``this`` to dynamic
 *   storage and returns a pointer to the internal ``mpz_t`` instance,
 * * an ``mpz_view`` class, an instance of which can be requested via the :cpp:func:`~mppp::integer::get_mpz_view()`
 *   method, which allows to use :cpp:class:`~mppp::integer` in the GMP API as a drop-in replacement for
 *   ``const mpz_t`` function arguments.
 *
 * The ``mpz_view`` class represent a read-only view of an integer object which is implicitly convertible to the type
 * ``const mpz_t`` and which is thus usable as an argument to GMP functions. For example:
 *
 * .. code-block:: c++
 *
 *    mpz_t m;
 *    mpz_init_set_si(m,1); // Create an mpz_t with the value 1.
 *    integer<1> n{1}; // Initialize an integer with the value 1.
 *    mpz_add(m,m,n.get_mpz_view()); // Compute the result of n + m and store
 *                                   // it in m using the GMP API.
 *
 * See the documentation of :cpp:func:`~mppp::integer::get_mpz_view()` for more details about the ``mpz_view`` class.
 * Via the GMP interfacing facilities, it is thus possible to use directly the GMP C API with
 * :cpp:class:`~mppp::integer` objects whenever necessary (e.g., if a GMP function has not been wrapped yet by mp++).
 * \endrststar
 */
template <std::size_t SSize>
class integer
{
    // Typedefs for ease of use.
    using s_storage = static_int<SSize>;
    using d_storage = mpz_struct_t;
    // The underlying static int.
    using s_int = s_storage;
    // mpz view class.
    // NOTE: this class looks more complicated than it seemingly should be - it looks like
    // it should be enough to shallow-copy around an mpz_struct_t with the limbs pointing
    // either to a static integer or a dynamic one. This option, however, has a problem:
    // in case of a dynamic integer, one can get 2 different mpz_struct_t instances, one
    // via m_int.g_dy(), the other via the view class, which internally point to the same
    // limbs vector. Having 2 *distinct* mpz_t point to the same limbs vector is not
    // something which is supported by the GMP API, at least when one mpz_t is being written into.
    // This happens for instance when using arithmetic functions
    // in which the return object is the same as one of the operands (GMP supports aliasing
    // if multiple mpz_t are the same object, but in this case they are distinct objects.)
    //
    // In the current implementation, mixing dynamic views with mpz_t objects in the GMP
    // API is fine, as dynamic views point to unique mpz_t objects within the integers.
    // We still however have potential for aliasing when using the static views: these
    // do not point to "real" mpz_t objects, they are mpz_t objects internally pointing
    // to the limbs of static integers. So, for instance, in an add() operation with
    // rop dynamic, and op1 and op2 statics and the same object, we create two separate
    // static views pointing to the same limbs internally, and feed them to the GMP API.
    //
    // This seems to work however: the data in the static views is strictly read-only and
    // it cannot be modified by a modification to a rop, as this needs to be a real mpz_t
    // in order to be used in the GMP API (views are convertible to const mpz_t only,
    // not mutable mpz_t). That is, static views and rops can only point to distinct
    // limbs vectors.
    //
    // The bottom line is that, in practice, the GMP API does not seem to be bothered by the fact
    // that const arguments might overlap behind its back. If this ever becomes a problem,
    // we can consider a "true" static view which does not simply point to an existing
    // static integer but actually copies it as a data member.
    struct mpz_view {
        explicit mpz_view(const integer &n)
            // NOTE: explicitly initialize mpz_struct_t() in order to avoid reading from
            // uninited memory in the move constructor, in case of a dynamic view.
            : m_static_view(n.is_static() ? n.m_int.g_st().get_mpz_view() : mpz_struct_t()),
              m_ptr(n.is_static() ? &m_static_view : &(n.m_int.g_dy()))
        {
        }
        mpz_view(const mpz_view &) = delete;
        mpz_view(mpz_view &&other)
            : m_static_view(other.m_static_view),
              // NOTE: we need to re-init ptr here if other is a static view, because in that case
              // other.m_ptr will be pointing to an mpz_struct_t in other and we want it to point to
              // the struct in this now.
              m_ptr((other.m_ptr == &other.m_static_view) ? &m_static_view : other.m_ptr)
        {
        }
        mpz_view &operator=(const mpz_view &) = delete;
        mpz_view &operator=(mpz_view &&) = delete;
        operator const mpz_struct_t *() const
        {
            return get();
        }
        const mpz_struct_t *get() const
        {
            return m_ptr;
        }
        mpz_struct_t m_static_view;
        const mpz_struct_t *m_ptr;
    };
    // Make friends with rational.
    template <std::size_t>
    friend class rational;

public:
    /// Alias for the template parameter \p SSize.
    static constexpr std::size_t ssize = SSize;
    /// Default constructor.
    /**
     * The default constructor initialises an integer with static storage type and value 0.
     */
    integer() = default;
    /// Copy constructor.
    /**
     * The copy constructor deep-copies \p other into \p this, copying the original storage type as well.
     *
     * @param other the object that will be copied into \p this.
     */
    integer(const integer &other) = default;
    /// Move constructor.
    /**
     * The move constructor will leave \p other in an unspecified but valid state. The storage type
     * of \p this will be the same as <tt>other</tt>'s.
     *
     * @param other the object that will be moved into \p this.
     */
    integer(integer &&other) = default;
/// Generic constructor.
/**
 * This constructor will initialize an integer with the value of \p x. The initialization is always
 * successful if \p x is an integral value (construction from \p bool yields 1 for \p true, 0 for \p false).
 * If \p x is a floating-point value, the construction will fail if \p x is not finite. Construction from a
 * floating-point type yields the truncated counterpart of the input value.
 *
 * @param x value that will be used to initialize \p this.
 *
 * @throws std::domain_error if \p x is a non-finite floating-point value.
 */
#if defined(MPPP_HAVE_CONCEPTS)
    explicit integer(const CppInteroperable &x)
#else
    template <typename T, cpp_interoperable_enabler<T> = 0>
    explicit integer(const T &x)
#endif
        : m_int(x)
    {
    }

private:
    // A tag to call private ctors.
    struct ptag {
    };
    explicit integer(const ptag &, const char *s, int base) : m_int(s, base) {}
    explicit integer(const ptag &, const std::string &s, int base) : integer(s.c_str(), base) {}
#if MPPP_CPLUSPLUS >= 201703L
    explicit integer(const ptag &, const std::string_view &s, int base) : integer(s.data(), s.data() + s.size(), base)
    {
    }
#endif
public:
    /// Constructor from string.
    /**
     * \rststar
     * This constructor will initialize ``this`` from the :cpp:concept:`~mppp::StringType` ``s``, which must represent
     * an integer value in base ``base``. The expected format is the same as specified by the ``mpz_set_str()``
     * GMP function. ``base`` may vary from 2 to 62, or be zero. In the latter case, the base is inferred
     * from the leading characters of the string.
     * \endrststar
     *
     * @param s the input string.
     * @param base the base used in the string representation.
     *
     * @throws std::invalid_argument if the \p base parameter is invalid or if \p s is not a valid string representation
     * of an integer in the specified base.
     * @throws unspecified any exception thrown by memory errors in standard containers.
     *
     * \rststar
     * .. seealso::
     *
     *    https://gmplib.org/manual/Assigning-Integers.html
     * \endrststar
     */
#if defined(MPPP_HAVE_CONCEPTS)
    explicit integer(const StringType &s,
#else
    template <typename T, string_type_enabler<T> = 0>
    explicit integer(const T &s,
#endif
                     int base = 10)
        : integer(ptag{}, s, base)
    {
    }
    /// Constructor from range of characters.
    /**
     * This constructor will initialise \p this from the content of the input half-open range,
     * which is interpreted as the string representation of an integer in base \p base.
     *
     * Internally, the constructor will copy the content of the range to a local buffer, add a
     * string terminator, and invoke the constructor from string.
     *
     * @param begin the begin of the input range.
     * @param end the end of the input range.
     * @param base the base used in the string representation.
     *
     * @throws unspecified any exception thrown by the constructor from string, or by memory
     * allocation errors in standard containers.
     */
    explicit integer(const char *begin, const char *end, int base = 10) : m_int(begin, end, base) {}
    /// Copy constructor from \p mpz_t.
    /**
     * This constructor will initialize \p this with the value of the GMP integer \p n. The storage type of \p this
     * will be static if \p n fits in the static storage, otherwise it will be dynamic.
     *
     * \rststar
     * .. warning::
     *
     *    It is the user's responsibility to ensure that ``n`` has been correctly initialized. Calling this constructor
     *    with an uninitialized ``n`` results in undefined behaviour.
     * \endrststar
     *
     * @param n the input GMP integer.
     */
    explicit integer(const ::mpz_t n) : m_int(n) {}
#if !defined(_MSC_VER)
    /// Move constructor from \p mpz_t.
    /**
     * This constructor will initialize \p this with the value of the GMP integer \p n, transferring the state
     * of \p n into \p this. The storage type of \p this
     * will be static if \p n fits in the static storage, otherwise it will be dynamic.
     *
     * \rststar
     * .. warning::
     *
     *    It is the user's responsibility to ensure that ``n`` has been correctly initialized. Calling this constructor
     *    with an uninitialized ``n`` results in undefined behaviour.
     *
     *    Additionally, the user must ensure that, after construction, ``mpz_clear()`` is never
     *    called on ``n``: the resources previously owned by ``n`` are now owned by ``this``, which
     *    will take care of releasing them when the destructor is called.
     *
     * .. note::
     *
     *    Due to a compiler bug, this constructor is not available on Microsoft Visual Studio.
     * \endrststar
     *
     * @param n the input GMP integer.
     */
    explicit integer(::mpz_t &&n) : m_int(std::move(n)) {}
#endif
    /// Copy assignment operator.
    /**
     * This operator will perform a deep copy of \p other, copying its storage type as well.
     *
     * @param other the assignment argument.
     *
     * @return a reference to \p this.
     */
    integer &operator=(const integer &other) = default;
    /// Move assignment operator.
    /**
     * After the move, \p other will be in an unspecified but valid state, and the storage type of \p this will be
     * <tt>other</tt>'s original storage type.
     *
     * @param other the assignment argument.
     *
     * @return a reference to \p this.
     */
    integer &operator=(integer &&other) = default;
/// Generic assignment operator.
/**
 * \rststar
 * The body of this operator is equivalent to:
 *
 * .. code-block:: c++
 *
 *    return *this = integer{x};
 *
 * That is, a temporary integer is constructed from ``x`` and it is then move-assigned to ``this``.
 * \endrststar
 *
 * @param x the assignment argument.
 *
 * @return a reference to \p this.
 *
 * @throws unspecified any exception thrown by the generic constructor.
 */
#if defined(MPPP_HAVE_CONCEPTS)
    integer &operator=(const CppInteroperable &x)
#else
    template <typename T, cpp_interoperable_enabler<T> = 0>
    integer &operator=(const T &x)
#endif
    {
        return *this = integer{x};
    }
        /// Assignment from string.
        /**
         * \rststar
         * The body of this operator is equivalent to:
         *
         * .. code-block:: c++
         *
         *    return *this = integer{s};
         *
         * That is, a temporary integer is constructed from the :cpp:concept:`~mppp::StringType`
         * ``s`` and it is then move-assigned to ``this``.
         * \endrststar
         *
         * @param s the string that will be used for the assignment.
         *
         * @return a reference to \p this.
         *
         * @throws unspecified any exception thrown by the constructor from string.
         */
#if defined(MPPP_HAVE_CONCEPTS)
    integer &operator=(const StringType &s)
#else
    template <typename T, string_type_enabler<T> = 0>
    integer &operator=(const T &s)
#endif
    {
        return *this = integer{s};
    }
    /// Copy assignment from \p mpz_t.
    /**
     * This assignment operator will copy into \p this the value of the GMP integer \p n. The storage type of \p this
     * after the assignment will be static if \p n fits in the static storage, otherwise it will be dynamic.
     *
     * \rststar
     * .. warning::
     *
     *    It is the user's responsibility to ensure that ``n`` has been correctly initialized. Calling this operator
     *    with an uninitialized ``n`` results in undefined behaviour. Also, no aliasing is allowed: the data in ``n``
     *    must be completely distinct from the data in ``this`` (e.g., if ``n`` is an ``mpz_view`` of ``this`` then
     *    it might point to internal data of ``this``, and the behaviour of this operator will thus be undefined).
     * \endrststar
     *
     * @param n the input GMP integer.
     *
     * @return a reference to \p this.
     */
    integer &operator=(const ::mpz_t n)
    {
        const auto asize = get_mpz_size(n);
        const auto s = is_static();
        if (s && asize <= SSize) {
            // this is static, n fits into static. Copy over.
            m_int.g_st()._mp_size = n->_mp_size;
            copy_limbs_no(n->_mp_d, n->_mp_d + asize, m_int.g_st().m_limbs.data());
            if (SSize <= static_int<SSize>::opt_size) {
                // Zero the non-copied limbs, but only if the static size is not greater than
                // the size for which special optimisations kick in. See the documentation
                // of zero_unused_limbs().
                std::fill(m_int.g_st().m_limbs.begin() + asize, m_int.g_st().m_limbs.end(), ::mp_limb_t(0));
            }
        } else if (!s && asize > SSize) {
            // Dynamic to dynamic.
            ::mpz_set(&m_int.m_dy, n);
        } else if (s && asize > SSize) {
            // this is static, n is too big. Promote and assign.
            // Destroy static.
            m_int.g_st().~s_storage();
            // Init dynamic.
            ::new (static_cast<void *>(&m_int.m_dy)) d_storage;
            mpz_init_set_nlimbs(m_int.m_dy, *n);
        } else {
            // This is dynamic and n fits into static.
            assert(!s && asize <= SSize);
            // Destroy the dynamic storage.
            m_int.destroy_dynamic();
            // Init a static with the content from n.
            ::new (static_cast<void *>(&m_int.m_st)) s_storage{n->_mp_size, n->_mp_d, asize};
        }
        return *this;
    }
#if !defined(_MSC_VER)
    /// Move assignment from \p mpz_t.
    /**
     * This assignment operator will move into \p this the GMP integer \p n. The storage type of \p this
     * after the assignment will be static if \p n fits in the static storage, otherwise it will be dynamic.
     *
     * \rststar
     * .. warning::
     *
     *    It is the user's responsibility to ensure that ``n`` has been correctly initialized. Calling this operator
     *    with an uninitialized ``n`` results in undefined behaviour. Also, no aliasing is allowed: the data in ``n``
     *    must be completely distinct from the data in ``this`` (e.g., if ``n`` is an ``mpz_view`` of ``this`` then
     *    it might point to internal data of ``this``, and the behaviour of this operator will thus be undefined).
     *
     *    Additionally, the user must ensure that, after the assignment, ``mpz_clear()`` is never
     *    called on ``n``: the resources previously owned by ``n`` are now owned by ``this``, which
     *    will take care of releasing them when the destructor is called.
     *
     * .. note::
     *
     *    Due to a compiler bug, this operator is not available on Microsoft Visual Studio.
     * \endrststar
     *
     * @param n the input GMP integer.
     *
     * @return a reference to \p this.
     */
    integer &operator=(::mpz_t &&n)
    {
        const auto asize = get_mpz_size(n);
        const auto s = is_static();
        if (s && asize <= SSize) {
            // this is static, n fits into static. Copy over.
            m_int.g_st()._mp_size = n->_mp_size;
            copy_limbs_no(n->_mp_d, n->_mp_d + asize, m_int.g_st().m_limbs.data());
            if (SSize <= static_int<SSize>::opt_size) {
                // Zero the non-copied limbs, but only if the static size is not greater than
                // the size for which special optimisations kick in. See the documentation
                // of zero_unused_limbs().
                std::fill(m_int.g_st().m_limbs.begin() + asize, m_int.g_st().m_limbs.end(), ::mp_limb_t(0));
            }
            // Clear out n.
            mpz_clear_wrap(*n);
        } else if (!s && asize > SSize) {
            // Dynamic to dynamic: clear this, shallow copy n.
            mpz_clear_wrap(m_int.m_dy);
            m_int.m_dy = *n;
        } else if (s && asize > SSize) {
            // this is static, n is too big. Promote and assign.
            // Destroy static.
            m_int.g_st().~s_storage();
            // Init dynamic with a shallow copy.
            ::new (static_cast<void *>(&m_int.m_dy)) d_storage(*n);
        } else {
            // This is dynamic and n fits into static.
            assert(!s && asize <= SSize);
            // Destroy the dynamic storage.
            m_int.destroy_dynamic();
            // Init a static with the content from n.
            ::new (static_cast<void *>(&m_int.m_st)) s_storage{n->_mp_size, n->_mp_d, asize};
            // Clear out n.
            mpz_clear_wrap(*n);
        }
        return *this;
    }
#endif
    /// Set to zero.
    /**
     * After calling this method, the storage type of \p this will be static and its value will be zero.
     *
     * \rststar
     * .. note::
     *
     *   This is a specialised higher-performance alternative to the assignment operator.
     * \endrststar
     *
     * @return a reference to \p this.
     */
    integer &set_zero()
    {
        if (is_static()) {
            m_int.g_st()._mp_size = 0;
            // Zero out the whole limbs array (equivalent to the def ctor of static int).
            std::fill(m_int.g_st().m_limbs.begin(), m_int.g_st().m_limbs.end(), ::mp_limb_t(0));
        } else {
            m_int.destroy_dynamic();
            // Def construction of static results in zero.
            ::new (static_cast<void *>(&m_int.m_st)) s_storage();
        }
        return *this;
    }

private:
    template <bool PlusOrMinus>
    integer &set_one_impl()
    {
        if (is_static()) {
            m_int.g_st()._mp_size = PlusOrMinus ? 1 : -1;
            m_int.g_st().m_limbs[0] = 1;
            // Zero the unused limbs, for the usual reason that optimised implementations
            // expect zeroes in unused limbs.
            std::fill(m_int.g_st().m_limbs.begin() + 1, m_int.g_st().m_limbs.end(), ::mp_limb_t(0));
        } else {
            m_int.destroy_dynamic();
            // Construct from a single limb the static. This will zero any unused limb.
            ::new (static_cast<void *>(&m_int.m_st)) s_storage{PlusOrMinus ? 1 : -1, 1u};
        }
        return *this;
    }

public:
    /// Set to one.
    /**
     * After calling this method, the storage type of \p this will be static and its value will be one.
     *
     * \rststar
     * .. note::
     *
     *   This is a specialised higher-performance alternative to the assignment operator.
     * \endrststar
     *
     * @return a reference to \p this.
     */
    integer &set_one()
    {
        return set_one_impl<true>();
    }
    /// Set to minus one.
    /**
     * After calling this method, the storage type of \p this will be static and its value will be minus one.
     *
     * \rststar
     * .. note::
     *
     *   This is a specialised higher-performance alternative to the assignment operator.
     * \endrststar
     *
     * @return a reference to \p this.
     */
    integer &set_negative_one()
    {
        return set_one_impl<false>();
    }
    /// Test for static storage.
    /**
     * @return \p true if the storage type is static, \p false otherwise.
     */
    bool is_static() const
    {
        return m_int.is_static();
    }
    /// Check for dynamic storage.
    /**
     * @return \p true if the storage type is dynamic, \p false otherwise.
     */
    bool is_dynamic() const
    {
        return m_int.is_dynamic();
    }
    /// Conversion to string.
    /**
     * This method will convert \p this into a string in base \p base using the GMP function \p mpz_get_str().
     *
     * @param base the desired base.
     *
     * @return a string representation of \p this.
     *
     * @throws std::invalid_argument if \p base is smaller than 2 or greater than 62.
     *
     * \rststar
     * .. seealso::
     *
     *    https://gmplib.org/manual/Converting-Integers.html
     * \endrststar
     */
    std::string to_string(int base = 10) const
    {
        if (mppp_unlikely(base < 2 || base > 62)) {
            throw std::invalid_argument("Invalid base for string conversion: the base must be between "
                                        "2 and 62, but a value of "
                                        + std::to_string(base) + " was provided instead");
        }
        return mpz_to_str(get_mpz_view(), base);
    }
    // NOTE: maybe provide a method to access the lower-level str conversion that writes to
    // std::vector<char>?

private:
    // Conversion to bool.
    template <typename T, enable_if_t<std::is_same<bool, T>::value, int> = 0>
    std::pair<bool, T> dispatch_conversion() const
    {
        return std::make_pair(true, m_int.m_st._mp_size != 0);
    }
    // Implementation of the conversion to unsigned types which fit in a limb.
    template <typename T, bool Sign, enable_if_t<(std::numeric_limits<T>::digits <= GMP_NUMB_BITS), int> = 0>
    std::pair<bool, T> convert_to_unsigned() const
    {
        static_assert(std::is_integral<T>::value && std::is_unsigned<T>::value, "Invalid type.");
        assert((Sign && m_int.m_st._mp_size > 0) || (!Sign && m_int.m_st._mp_size < 0));
        if ((Sign && m_int.m_st._mp_size != 1) || (!Sign && m_int.m_st._mp_size != -1)) {
            // If the asize is not 1, the conversion will fail.
            return std::make_pair(false, T(0));
        }
        // Get the pointer to the limbs.
        const ::mp_limb_t *ptr = is_static() ? m_int.g_st().m_limbs.data() : m_int.g_dy()._mp_d;
        if ((ptr[0] & GMP_NUMB_MASK) > std::numeric_limits<T>::max()) {
            // The only limb has a value which exceeds the limit of T.
            return std::make_pair(false, T(0));
        }
        // There's a single limb and the result fits.
        return std::make_pair(true, static_cast<T>(ptr[0] & GMP_NUMB_MASK));
    }
    // Implementation of the conversion to unsigned types which do not fit in a limb.
    template <typename T, bool Sign, enable_if_t<(std::numeric_limits<T>::digits > GMP_NUMB_BITS), int> = 0>
    std::pair<bool, T> convert_to_unsigned() const
    {
        static_assert(std::is_integral<T>::value && std::is_unsigned<T>::value, "Invalid type.");
        assert((Sign && m_int.m_st._mp_size > 0) || (!Sign && m_int.m_st._mp_size < 0));
        const auto asize = Sign ? static_cast<std::size_t>(m_int.m_st._mp_size)
                                : static_cast<std::size_t>(nint_abs(m_int.m_st._mp_size));
        // Get the pointer to the limbs.
        const ::mp_limb_t *ptr = is_static() ? m_int.g_st().m_limbs.data() : m_int.g_dy()._mp_d;
        // Init the retval with the first limb. This is safe as T has more bits than the limb type.
        auto retval = static_cast<T>(ptr[0] & GMP_NUMB_MASK);
        // Add the other limbs, if any.
        constexpr unsigned u_bits = std::numeric_limits<T>::digits;
        unsigned shift(GMP_NUMB_BITS);
        for (std::size_t i = 1u; i < asize; ++i, shift += unsigned(GMP_NUMB_BITS)) {
            if (shift >= u_bits) {
                // We need to shift left the current limb. If the shift is too large, we run into UB
                // and it also means that the value does not fit in T.
                return std::make_pair(false, T(0));
            }
            // Get the current limb. Safe as T has more bits than the limb type.
            const auto l = static_cast<T>(ptr[i] & GMP_NUMB_MASK);
            // LCOV_EXCL_START
            if (l >> (u_bits - shift)) {
                // Left-shifting the current limb is well-defined from the point of view of the language, but the result
                // overflows: the value does not fit in T.
                // NOTE: I suspect this branch can be triggered on common architectures only with nail builds.
                return std::make_pair(false, T(0));
            }
            // LCOV_EXCL_STOP
            // This will not overflow, as there is no carry from retval and l << shift is fine.
            retval = static_cast<T>(retval + (l << shift));
        }
        return std::make_pair(true, retval);
    }
    // Conversion to unsigned ints, excluding bool.
    template <typename T,
              enable_if_t<conjunction<std::is_integral<T>, std::is_unsigned<T>, negation<std::is_same<bool, T>>>::value,
                          int> = 0>
    std::pair<bool, T> dispatch_conversion() const
    {
        // Handle zero.
        if (!m_int.m_st._mp_size) {
            return std::make_pair(true, T(0));
        }
        // Handle negative value.
        if (m_int.m_st._mp_size < 0) {
            return std::make_pair(false, T(0));
        }
        return convert_to_unsigned<T, true>();
    }
    // Conversion to signed ints.
    //
    // NOTE: the implementation of conversion to signed at the moment is split into 2 branches:
    // a specialised implementation for T not larger than the limb type, and a slower implementation
    // for T larger than the limb type. The slower implementation is based on the conversion
    // to the unsigned counterpart of T, and it can probably be improved performance-wise.
    //
    // Helper type trait to detect conversion to small signed integers (i.e., the absolute values of T fit
    // into a limb). We need this instead of just typedeffing an std::integral_constant because MSVC
    // chokes on constexpr functions in a SFINAE context.
    template <typename T>
    struct sconv_is_small {
        static const bool value = c_max(static_cast<make_unsigned_t<T>>(std::numeric_limits<T>::max()),
                                        nint_abs(std::numeric_limits<T>::min()))
                                  <= GMP_NUMB_MAX;
    };
    // Overload if the all the absolute values of T fit into a limb.
    template <typename T, enable_if_t<sconv_is_small<T>::value, int> = 0>
    std::pair<bool, T> convert_to_signed() const
    {
        static_assert(std::is_integral<T>::value && std::is_signed<T>::value, "Invalid type.");
        assert(size());
        // Cache for convenience.
        constexpr auto Tmax = static_cast<make_unsigned_t<T>>(std::numeric_limits<T>::max());
        if (m_int.m_st._mp_size != 1 && m_int.m_st._mp_size != -1) {
            // this consists of more than 1 limb, the conversion is not possible.
            return std::make_pair(false, T(0));
        }
        // Get the pointer to the limbs.
        const ::mp_limb_t *ptr = is_static() ? m_int.g_st().m_limbs.data() : m_int.g_dy()._mp_d;
        // The candidate output value.
        const ::mp_limb_t candidate = ptr[0] & GMP_NUMB_MASK;
        // Branch out on the sign.
        if (m_int.m_st._mp_size > 0) {
            // This is positive, it needs to fit within max().
            if (candidate <= Tmax) {
                return std::make_pair(true, static_cast<T>(candidate));
            }
            return std::make_pair(false, T(0));
        } else {
            return unsigned_to_nsigned<T>(candidate);
        }
    }
    // Overload if not all the absolute values of T fit into a limb.
    template <typename T, enable_if_t<!sconv_is_small<T>::value, int> = 0>
    std::pair<bool, T> convert_to_signed() const
    {
        // Cache for convenience.
        constexpr auto Tmax = static_cast<make_unsigned_t<T>>(std::numeric_limits<T>::max());
        // Branch out depending on the sign of this.
        if (m_int.m_st._mp_size > 0) {
            // Attempt conversion to the unsigned counterpart.
            const auto candidate = convert_to_unsigned<make_unsigned_t<T>, true>();
            if (candidate.first && candidate.second <= Tmax) {
                // The conversion to unsigned was successful, and the result fits in
                // the positive range of T. Return the result.
                return std::make_pair(true, static_cast<T>(candidate.second));
            }
            // The conversion to unsigned failed, or the result does not fit.
            return std::make_pair(false, T(0));
        } else {
            // Attempt conversion to the unsigned counterpart.
            const auto candidate = convert_to_unsigned<make_unsigned_t<T>, false>();
            if (candidate.first) {
                // The converstion to unsigned was successful, try to negate now.
                return unsigned_to_nsigned<T>(candidate.second);
            }
            // The conversion to unsigned failed.
            return std::make_pair(false, T(0));
        }
    }
    template <typename T, enable_if_t<conjunction<std::is_integral<T>, std::is_signed<T>>::value, int> = 0>
    std::pair<bool, T> dispatch_conversion() const
    {
        // Handle zero.
        if (!m_int.m_st._mp_size) {
            return std::make_pair(true, T(0));
        }
        return convert_to_signed<T>();
    }
    // Implementation of the conversion to floating-point through GMP/MPFR routines.
    template <typename T, enable_if_t<disjunction<std::is_same<T, float>, std::is_same<T, double>>::value, int> = 0>
    static std::pair<bool, T> mpz_float_conversion(const mpz_struct_t &m)
    {
        return std::make_pair(true, static_cast<T>(::mpz_get_d(&m)));
    }
#if defined(MPPP_WITH_MPFR)
    template <typename T, enable_if_t<std::is_same<T, long double>::value, int> = 0>
    static std::pair<bool, T> mpz_float_conversion(const mpz_struct_t &m)
    {
        constexpr int d2 = std::numeric_limits<long double>::max_digits10 * 4;
        MPPP_MAYBE_TLS mpfr_raii mpfr(static_cast<::mpfr_prec_t>(d2));
        ::mpfr_set_z(&mpfr.m_mpfr, &m, MPFR_RNDN);
        return std::make_pair(true, ::mpfr_get_ld(&mpfr.m_mpfr, MPFR_RNDN));
    }
#endif
    // Conversion to floating-point.
    template <typename T, enable_if_t<std::is_floating_point<T>::value, int> = 0>
    std::pair<bool, T> dispatch_conversion() const
    {
        // Handle zero.
        if (!m_int.m_st._mp_size) {
            return std::make_pair(true, T(0));
        }
        if (std::numeric_limits<T>::is_iec559) {
            // Optimization for single-limb integers.
            //
            // NOTE: the reasoning here is as follows. If the floating-point type has infinity,
            // then its "range" is the whole real line. The C++ standard guarantees that:
            // """
            // A prvalue of an integer type or of an unscoped enumeration type can be converted to a prvalue of a
            // floating point type. The result is exact if possible. If the value being converted is in the range
            // of values that can be represented but the value cannot be represented exactly,
            // it is an implementation-defined choice of either the next lower or higher representable value. If the
            // value being converted is outside the range of values that can be represented, the behavior is undefined.
            // """
            // This seems to indicate that if the limb value "overflows" the finite range of the floating-point type, we
            // will get either the max/min finite value or +-inf. Additionally, the IEEE standard seems to indicate
            // that an overflowing conversion will produce infinity:
            // http://stackoverflow.com/questions/40694384/integer-to-float-conversions-with-ieee-fp
            //
            // Get the pointer to the limbs.
            const ::mp_limb_t *ptr = is_static() ? m_int.g_st().m_limbs.data() : m_int.g_dy()._mp_d;
            if (m_int.m_st._mp_size == 1) {
                return std::make_pair(true, static_cast<T>(ptr[0] & GMP_NUMB_MASK));
            }
            if (m_int.m_st._mp_size == -1) {
                return std::make_pair(true, -static_cast<T>(ptr[0] & GMP_NUMB_MASK));
            }
        }
        // For all the other cases, just delegate to the GMP/MPFR routines.
        return mpz_float_conversion<T>(*static_cast<const mpz_struct_t *>(get_mpz_view()));
    }

public:
/// Generic conversion operator.
/**
 * \rststar
 * This operator will convert ``this`` to a :cpp:concept:`~mppp::CppInteroperable` type.
 * Conversion to ``bool`` yields ``false`` if ``this`` is zero,
 * ``true`` otherwise. Conversion to other integral types yields the exact result, if representable by the target
 * :cpp:concept:`~mppp::CppInteroperable` type. Conversion to floating-point types might yield inexact values and
 * infinities.
 * \endrststar
 *
 * @return \p this converted to the target type.
 *
 * @throws std::overflow_error if the target type is an integral type and the value of ``this`` cannot be represented by
 * it.
 */
#if defined(MPPP_HAVE_CONCEPTS)
    template <CppInteroperable T>
#else
    template <typename T, cpp_interoperable_enabler<T> = 0>
#endif
    explicit operator T() const
    {
        auto retval = dispatch_conversion<T>();
        if (mppp_unlikely(!retval.first)) {
            throw std::overflow_error("Conversion of the integer " + to_string() + " to the type " + typeid(T).name()
                                      + " results in overflow");
        }
        return std::move(retval.second);
    }
        /// Generic conversion method.
        /**
         * \rststar
         * This method, similarly to the conversion operator, will convert ``this`` to a
         * :cpp:concept:`~mppp::CppInteroperable` type, storing the result of the conversion into ``rop``. Differently
         * from the conversion operator, this method does not raise any exception: if the conversion is successful, the
         * method will return ``true``, otherwise the method will return ``false``. If the conversion fails,
         * ``rop`` will not be altered.
         * \endrststar
         *
         * @param rop the variable which will store the result of the conversion.
         *
         * @return ``true`` if the conversion succeeded, ``false`` otherwise. The conversion can fail only if ``rop`` is
         * a C++ integral which cannot represent the value of ``this``.
         */
#if defined(MPPP_HAVE_CONCEPTS)
    template <CppInteroperable T>
#else
    template <typename T, cpp_interoperable_enabler<T> = 0>
#endif
    bool get(T &rop) const
    {
        auto retval = dispatch_conversion<T>();
        if (retval.first) {
            rop = std::move(retval.second);
            return true;
        }
        return false;
    }
    /// Promote to dynamic storage.
    /**
     * This method will promote the storage type of \p this from static to dynamic.
     *
     * @return \p false if the storage type of \p this is already dynamic and no promotion takes place, \p true
     * otherwise.
     */
    bool promote()
    {
        if (is_static()) {
            m_int.promote();
            return true;
        }
        return false;
    }
    /// Demote to static storage.
    /**
     * This method will demote the storage type of \p this from dynamic to static.
     *
     * @return \p false if the storage type of \p this is already static and no demotion takes place, or if the current
     * value of \p this does not fit in static storage, \p true otherwise.
     */
    bool demote()
    {
        if (is_dynamic()) {
            return m_int.demote();
        }
        return false;
    }
    /// Size in bits.
    /**
     * @return the number of bits needed to represent \p this. If \p this is zero, zero will be returned.
     *
     * @throws std::overflow_error if the size in bits of \p this is larger than an implementation-defined value.
     */
    std::size_t nbits() const
    {
#if defined(__clang__) || defined(__GNUC__)
        const std::size_t ls = size();
        const ::mp_limb_t *lptr = is_static() ? m_int.g_st().m_limbs.data() : m_int.g_dy()._mp_d;
        if (ls) {
            // LCOV_EXCL_START
            if (mppp_unlikely(ls > std::numeric_limits<std::size_t>::max() / unsigned(GMP_NUMB_BITS))) {
                throw std::overflow_error("Overflow in the computation of the number of bits required to represent an "
                                          "integer - the limb size is "
                                          + std::to_string(ls));
            }
            // LCOV_EXCL_STOP
            // Index of the most significant limb.
            const std::size_t idx = ls - 1u;
            // The most significant limb.
            const ::mp_limb_t msl = lptr[idx] & GMP_NUMB_MASK;
            // NOTE: here we need std::numeric_limits<::mp_limb_t>::digits (instead of GMP_NUMB_BITS) because
            // builtin_clz() counts zeroes also in the nail bits.
            return static_cast<std::size_t>(idx * unsigned(GMP_NUMB_BITS)
                                            + (unsigned(std::numeric_limits<::mp_limb_t>::digits) - builtin_clz(msl)));
        }
        return 0;
#else
        return m_int.m_st._mp_size ? ::mpz_sizeinbase(get_mpz_view(), 2) : 0u;
#endif
    }
    /// Size in limbs.
    /**
     * @return the number of limbs needed to represent \p this. If \p this is zero, zero will be returned.
     */
    std::size_t size() const
    {
        return (m_int.m_st._mp_size) >= 0 ? static_cast<std::size_t>(m_int.m_st._mp_size)
                                          : static_cast<std::size_t>(nint_abs(m_int.m_st._mp_size));
    }
    /// Sign.
    /**
     * @return 0 if \p this is zero, 1 if \p this is positive, -1 if \p this is negative.
     */
    int sgn() const
    {
        // NOTE: size is part of the common initial sequence.
        if (m_int.m_st._mp_size != 0) {
            return m_int.m_st._mp_size > 0 ? 1 : -1;
        } else {
            return 0;
        }
    }
    /// Get an \p mpz_t view.
    /**
     * This method will return an object of an unspecified type \p mpz_view which is implicitly convertible
     * to a const pointer to an \p mpz_t struct (and which can thus be used as a <tt>const mpz_t</tt>
     * parameter in GMP functions). In addition to the implicit conversion operator, the <tt>const mpz_t</tt>
     * object can also be retrieved via the <tt>get()</tt> method of the \p mpz_view class.
     * The view provides a read-only GMP-compatible representation of the integer stored in \p this.
     *
     * \rststar
     * .. note::
     *
     *   It is important to keep in mind the following facts about the returned ``mpz_view`` object:
     *
     *   * ``mpz_view`` objects are strictly read-only: it is impossible to alter ``this`` through an ``mpz_view``, and
     *     ``mpz_view`` objects can be used in the GMP API only where a ``const mpz_t`` parameter is expected;
     *   * ``mpz_view`` objects can only be move-constructed (the other constructors and the assignment operators
     *     are disabled);
     *   * the returned object and the pointer returned by its ``get()`` method might reference internal data
     *     belonging to ``this``, and they can thus be used safely only during the lifetime of ``this``;
     *   * the lifetime of the pointer returned by the ``get()`` method is tied to the lifetime of the ``mpz_view``
     *     object (that is, if the ``mpz_view`` object is destroyed, any pointer previously returned by ``get()``
     *     becomes invalid);
     *   * any modification to ``this`` will also invalidate the view and the pointer.
     * \endrststar
     *
     * @return an \p mpz view of \p this.
     */
    mpz_view get_mpz_view() const
    {
        return mpz_view(*this);
    }
    /// Negate in-place.
    /**
     * This method will set \p this to <tt>-this</tt>.
     *
     * @return a reference to \p this.
     */
    integer &neg()
    {
        m_int.neg();
        return *this;
    }
    /// In-place absolute value.
    /**
     * This method will set \p this to its absolute value.
     *
     * @return a reference to \p this.
     */
    integer &abs()
    {
        if (is_static()) {
            if (m_int.g_st()._mp_size < 0) {
                m_int.g_st()._mp_size = -m_int.g_st()._mp_size;
            }
        } else {
            ::mpz_abs(&m_int.g_dy(), &m_int.g_dy());
        }
        return *this;
    }
    /// Compute next prime number (in-place version).
    /**
     * This method will set \p this to the first prime number greater than the current value.
     *
     * @return a reference to \p this.
     */
    integer &nextprime()
    {
        nextprime_impl(*this, *this);
        return *this;
    }
    /// Test primality.
    /**
     * This method will run a series of probabilistic tests to determine if \p this is a prime number.
     * It will return \p 2 if \p this is definitely a prime, \p 1 if \p this is probably a prime and \p 0 if \p this
     * is definitely not-prime.
     *
     * @param reps the number of tests to run.
     *
     * @return an integer indicating if \p this is a prime.
     *
     * @throws std::invalid_argument if \p reps is less than 1 or if \p this is negative.
     */
    int probab_prime_p(int reps = 25) const
    {
        if (mppp_unlikely(reps < 1)) {
            throw std::invalid_argument("The number of primality tests must be at least 1, but a value of "
                                        + std::to_string(reps) + " was provided instead");
        }
        if (mppp_unlikely(sgn() < 0)) {
            throw std::invalid_argument("Cannot run primality tests on the negative number " + to_string());
        }
        return ::mpz_probab_prime_p(get_mpz_view(), reps);
    }
    /// Integer square root (in-place version).
    /**
     * This method will set \p this to its integer square root.
     *
     * @return a reference to \p this.
     *
     * @throws std::domain_error if \p this is negative.
     */
    integer &sqrt()
    {
        mppp::sqrt(*this, *this);
        return *this;
    }
    /// Test if value is odd.
    /**
     * @return \p true if \p this is odd, \p false otherwise.
     */
    bool odd_p() const
    {
        if (is_static()) {
            // NOTE: as usual we assume that a zero static integer has all limbs set to zero.
            return (m_int.g_st().m_limbs[0] & GMP_NUMB_MASK) & ::mp_limb_t(1);
        }
        return mpz_odd_p(&m_int.g_dy());
    }
    /// Test if value is even.
    /**
     * @return \p true if \p this is even, \p false otherwise.
     */
    bool even_p() const
    {
        return !odd_p();
    }
    /// Return a reference to the internal union.
    /**
     * This method returns a reference to the union used internally to implement the integer class.
     *
     * @return a reference to the internal union member.
     */
    integer_union<SSize> &_get_union()
    {
        return m_int;
    }
    /// Return a const reference to the internal union.
    /**
     * This method returns a const reference to the union used internally to implement the integer class.
     *
     * @return a const reference to the internal union member.
     */
    const integer_union<SSize> &_get_union() const
    {
        return m_int;
    }
    /// Get a pointer to the dynamic storage.
    /**
     * This method will first promote \p this to dynamic storage (if \p this is not already employing dynamic storage),
     * and it will then return a pointer to the internal \p mpz_t structure. The returned pointer can be used as an
     * argument for the functions of the GMP API.
     *
     * \rststar
     * .. note::
     *
     *    The returned pointer is a raw, non-owning pointer tied to the lifetime of ``this``. Calling
     *    :cpp:func:`~mppp::integer::demote()` or
     *    assigning an :cpp:class:`~mppp::integer` with static storage to ``this`` will invalidate the returned
     *    pointer.
     * \endrststar
     *
     * @return a pointer to the internal \p mpz_t structure.
     */
    std::remove_extent<::mpz_t>::type *get_mpz_t()
    {
        promote();
        return &m_int.g_dy();
    }
    /// Test if the value is zero.
    /**
     * @return \p true if the value represented by \p this is zero, \p false otherwise.
     */
    bool is_zero() const
    {
        return m_int.m_st._mp_size == 0;
    }

private:
    // Implementation of is_one()/is_negative_one().
    template <int One>
    bool is_one_impl() const
    {
        if (m_int.m_st._mp_size != One) {
            return false;
        }
        // Get the pointer to the limbs.
        const ::mp_limb_t *ptr = is_static() ? m_int.g_st().m_limbs.data() : m_int.g_dy()._mp_d;
        return (ptr[0] & GMP_NUMB_MASK) == 1u;
    }

public:
    /// Test if the value is equal to one.
    /**
     * @return \p true if the value represented by \p this is 1, \p false otherwise.
     */
    bool is_one() const
    {
        return is_one_impl<1>();
    }
    /// Test if the value is equal to minus one.
    /**
     * @return \p true if the value represented by \p this is -1, \p false otherwise.
     */
    bool is_negative_one() const
    {
        return is_one_impl<-1>();
    }

private:
    integer_union<SSize> m_int;
};

#if MPPP_CPLUSPLUS < 201703L

// NOTE: from C++17 static constexpr members are implicitly inline, and it's not necessary
// any more (actually, it's deprecated) to re-declare them outside the class.
// https://stackoverflow.com/questions/39646958/constexpr-static-member-before-after-c17

template <std::size_t SSize>
constexpr std::size_t integer<SSize>::ssize;

#endif

/** @defgroup integer_assignment integer_assignment
 *  @{
 */

/// Set to zero.
/**
 * After calling this function, the storage type of \p n will be static and its value will be zero.
 *
 * \rststar
 * .. note::
 *
 *   This is a specialised higher-performance alternative to the assignment operator.
 * \endrststar
 *
 * @param n the assignment argument.
 *
 * @return a reference to \p n.
 */
template <std::size_t SSize>
inline integer<SSize> &set_zero(integer<SSize> &n)
{
    return n.set_zero();
}

/// Set to one.
/**
 * After calling this function, the storage type of \p n will be static and its value will be one.
 *
 * \rststar
 * .. note::
 *
 *   This is a specialised higher-performance alternative to the assignment operator.
 * \endrststar
 *
 * @param n the assignment argument.
 *
 * @return a reference to \p n.
 */
template <std::size_t SSize>
inline integer<SSize> &set_one(integer<SSize> &n)
{
    return n.set_one();
}

/// Set to minus one.
/**
 * After calling this function, the storage type of \p n will be static and its value will be minus one.
 *
 * \rststar
 * .. note::
 *
 *   This is a specialised higher-performance alternative to the assignment operator.
 * \endrststar
 *
 * @param n the assignment argument.
 *
 * @return a reference to \p n.
 */
template <std::size_t SSize>
inline integer<SSize> &set_negative_one(integer<SSize> &n)
{
    return n.set_negative_one();
}

/** @} */

/** @defgroup integer_conversion integer_conversion
 *  @{
 */

/// Generic conversion function for \link mppp::integer integer\endlink.
/**
 * \rststar
 * This function will convert the input :cpp:class:`~mppp::integer` ``n`` to a
 * :cpp:concept:`~mppp::CppInteroperable` type, storing the result of the conversion into ``rop``.
 * If the conversion is successful, the function
 * will return ``true``, otherwise the function will return ``false``. If the conversion fails, ``rop`` will
 * not be altered.
 * \endrststar
 *
 * @param rop the variable which will store the result of the conversion.
 * @param n the input \link mppp::integer integer\endlink.
 *
 * @return ``true`` if the conversion succeeded, ``false`` otherwise. The conversion can fail only if ``rop`` is
 * a C++ integral which cannot represent the value of ``n``.
 */
#if defined(MPPP_HAVE_CONCEPTS)
template <std::size_t SSize>
inline bool get(CppInteroperable &rop, const integer<SSize> &n)
#else
template <typename T, std::size_t SSize, cpp_interoperable_enabler<T> = 0>
inline bool get(T &rop, const integer<SSize> &n)
#endif
{
    return n.get(rop);
}

/** @} */

inline namespace detail
{

// Machinery for the determination of the result of a binary operation involving integer.
// Default is empty for SFINAE.
template <typename, typename, typename = void>
struct integer_common_type {
};

template <std::size_t SSize>
struct integer_common_type<integer<SSize>, integer<SSize>> {
    using type = integer<SSize>;
};

template <std::size_t SSize, typename U>
struct integer_common_type<integer<SSize>, U, enable_if_t<is_supported_integral<U>::value>> {
    using type = integer<SSize>;
};

template <std::size_t SSize, typename T>
struct integer_common_type<T, integer<SSize>, enable_if_t<is_supported_integral<T>::value>> {
    using type = integer<SSize>;
};

template <std::size_t SSize, typename U>
struct integer_common_type<integer<SSize>, U, enable_if_t<is_supported_float<U>::value>> {
    using type = U;
};

template <std::size_t SSize, typename T>
struct integer_common_type<T, integer<SSize>, enable_if_t<is_supported_float<T>::value>> {
    using type = T;
};

template <typename T, typename U>
using integer_common_t = typename integer_common_type<T, U>::type;

// Various utilities, concepts, enablers used in both the operators and the functions.
template <typename T, typename U>
struct is_same_ssize_integer : std::false_type {
};

template <std::size_t SSize>
struct is_same_ssize_integer<integer<SSize>, integer<SSize>> : std::true_type {
};

template <typename T>
struct is_integer : std::false_type {
};

template <std::size_t SSize>
struct is_integer<integer<SSize>> : std::true_type {
};
}

template <typename T, typename U>
#if defined(MPPP_HAVE_CONCEPTS)
concept bool IntegerOpTypes
    = is_same_ssize_integer<T, U>::value
      || (is_integer<T>::value && CppInteroperable<U>) || (is_integer<U>::value && CppInteroperable<T>);
#else
using integer_op_types_enabler
    = enable_if_t<disjunction<is_same_ssize_integer<T, U>, conjunction<is_integer<T>, is_cpp_interoperable<U>>,
                              conjunction<is_integer<U>, is_cpp_interoperable<T>>>::value,
                  int>;
#endif

template <typename T, typename U>
#if defined(MPPP_HAVE_CONCEPTS)
concept bool IntegerIntegralOpTypes
    = is_same_ssize_integer<T, U>::value || (is_integer<T>::value && CppInteroperable<U> && std::is_integral<U>::value)
      || (is_integer<U>::value && CppInteroperable<T> && std::is_integral<T>::value);
#else
using integer_integral_op_types_enabler = enable_if_t<
    disjunction<is_same_ssize_integer<T, U>, conjunction<is_integer<T>, is_cpp_interoperable<U>, std::is_integral<U>>,
                conjunction<is_integer<U>, is_cpp_interoperable<T>, std::is_integral<T>>>::value,
    int>;
#endif

/** @defgroup integer_arithmetic integer_arithmetic
 *  @{
 */

inline namespace detail
{

// Metaprogramming for selecting the algorithm for static addition. The selection happens via
// an std::integral_constant with 3 possible values:
// - 0 (default case): use the GMP mpn functions,
// - 1: selected when there are no nail bits and the static size is 1,
// - 2: selected when there are no nail bits and the static size is 2.
template <typename SInt>
using integer_static_add_algo = std::integral_constant<
    int, (!GMP_NAIL_BITS && SInt::s_size == 1) ? 1 : ((!GMP_NAIL_BITS && SInt::s_size == 2) ? 2 : 0)>;

// General implementation via mpn.
// Small helper to compute the size after subtraction via mpn. s is a strictly positive size.
inline mpz_size_t integer_sub_compute_size(const ::mp_limb_t *rdata, mpz_size_t s)
{
    assert(s > 0);
    mpz_size_t cur_idx = s - 1;
    for (; cur_idx >= 0; --cur_idx) {
        if (rdata[cur_idx] & GMP_NUMB_MASK) {
            break;
        }
    }
    return cur_idx + 1;
}

// NOTE: this function (and its other overloads) will return true in case of success, false in case of failure
// (i.e., the addition overflows and the static size is not enough).
template <std::size_t SSize>
inline bool static_add_impl(static_int<SSize> &rop, const static_int<SSize> &op1, const static_int<SSize> &op2,
                            mpz_size_t asize1, mpz_size_t asize2, int sign1, int sign2,
                            const std::integral_constant<int, 0> &)
{
    auto rdata = &rop.m_limbs[0];
    auto data1 = &op1.m_limbs[0], data2 = &op2.m_limbs[0];
    const auto size1 = op1._mp_size;
    // NOTE: cannot trust the size member from op2, as op2 could've been negated if
    // we are actually subtracting.
    const auto size2 = (sign2 >= 0) ? asize2 : -asize2;
    // mpn functions require nonzero arguments.
    if (mppp_unlikely(!sign2)) {
        rop._mp_size = size1;
        copy_limbs(data1, data1 + asize1, rdata);
        return true;
    }
    if (mppp_unlikely(!sign1)) {
        rop._mp_size = size2;
        copy_limbs(data2, data2 + asize2, rdata);
        return true;
    }
    // Check, for op1 and op2, whether:
    // - the asize is the max static size, and, if yes,
    // - the highest bit in the highest limb is set.
    // If this condition holds for op1 and op2, we return failure, as the computation might require
    // an extra limb.
    // NOTE: the reason we check this is that we do not want to run into the situation in which we have written
    // something into rop (via the mpn functions below), only to realize later that the computation overflows.
    // This would be bad because, in case of overlapping arguments, it would destroy one or two operands without
    // possibility of recovering. The alternative would be to do the computation in some local buffer and then
    // copy it out, but that is rather costly. Note that this means that in principle a computation that could fit in
    // static storage ends up triggering a promotion.
    const bool c1 = std::size_t(asize1) == SSize && ((data1[asize1 - 1] & GMP_NUMB_MASK) >> (GMP_NUMB_BITS - 1));
    const bool c2 = std::size_t(asize2) == SSize && ((data2[asize2 - 1] & GMP_NUMB_MASK) >> (GMP_NUMB_BITS - 1));
    if (mppp_unlikely(c1 || c2)) {
        return false;
    }
    if (sign1 == sign2) {
        // Same sign.
        if (asize1 >= asize2) {
            // The number of limbs of op1 >= op2.
            ::mp_limb_t cy;
            if (asize2 == 1) {
                // NOTE: we are not masking data2[0] with GMP_NUMB_MASK, I am assuming the mpn function
                // is able to deal with a limb with a nail.
                cy = ::mpn_add_1(rdata, data1, static_cast<::mp_size_t>(asize1), data2[0]);
            } else if (asize1 == asize2) {
                cy = ::mpn_add_n(rdata, data1, data2, static_cast<::mp_size_t>(asize1));
            } else {
                cy = ::mpn_add(rdata, data1, static_cast<::mp_size_t>(asize1), data2, static_cast<::mp_size_t>(asize2));
            }
            if (cy) {
                assert(asize1 < static_int<SSize>::s_size);
                rop._mp_size = size1 + sign1;
                // NOTE: there should be no need to use GMP_NUMB_MASK here.
                rdata[asize1] = 1u;
            } else {
                // Without carry, the size is unchanged.
                rop._mp_size = size1;
            }
        } else {
            // The number of limbs of op2 > op1.
            ::mp_limb_t cy;
            if (asize1 == 1) {
                cy = ::mpn_add_1(rdata, data2, static_cast<::mp_size_t>(asize2), data1[0]);
            } else {
                cy = ::mpn_add(rdata, data2, static_cast<::mp_size_t>(asize2), data1, static_cast<::mp_size_t>(asize1));
            }
            if (cy) {
                assert(asize2 < static_int<SSize>::s_size);
                rop._mp_size = size2 + sign2;
                rdata[asize2] = 1u;
            } else {
                rop._mp_size = size2;
            }
        }
    } else {
        if (asize1 > asize2 || (asize1 == asize2 && ::mpn_cmp(data1, data2, static_cast<::mp_size_t>(asize1)) >= 0)) {
            // abs(op1) >= abs(op2).
            ::mp_limb_t br;
            if (asize2 == 1) {
                br = ::mpn_sub_1(rdata, data1, static_cast<::mp_size_t>(asize1), data2[0]);
            } else if (asize1 == asize2) {
                br = ::mpn_sub_n(rdata, data1, data2, static_cast<::mp_size_t>(asize1));
            } else {
                br = ::mpn_sub(rdata, data1, static_cast<::mp_size_t>(asize1), data2, static_cast<::mp_size_t>(asize2));
            }
            assert(!br);
            rop._mp_size = integer_sub_compute_size(rdata, asize1);
            if (sign1 != 1) {
                rop._mp_size = -rop._mp_size;
            }
        } else {
            // abs(op2) > abs(op1).
            ::mp_limb_t br;
            if (asize1 == 1) {
                br = ::mpn_sub_1(rdata, data2, static_cast<::mp_size_t>(asize2), data1[0]);
            } else {
                br = ::mpn_sub(rdata, data2, static_cast<::mp_size_t>(asize2), data1, static_cast<::mp_size_t>(asize1));
            }
            assert(!br);
            rop._mp_size = integer_sub_compute_size(rdata, asize2);
            if (sign2 != 1) {
                rop._mp_size = -rop._mp_size;
            }
        }
    }
    return true;
}

// Optimization for single-limb statics with no nails.
template <std::size_t SSize>
inline bool static_add_impl(static_int<SSize> &rop, const static_int<SSize> &op1, const static_int<SSize> &op2,
                            mpz_size_t asize1, mpz_size_t asize2, int sign1, int sign2,
                            const std::integral_constant<int, 1> &)
{
    auto rdata = &rop.m_limbs[0];
    auto data1 = &op1.m_limbs[0], data2 = &op2.m_limbs[0];
    // NOTE: both asizes have to be 0 or 1 here.
    assert((asize1 == 1 && data1[0] != 0u) || (asize1 == 0 && data1[0] == 0u));
    assert((asize2 == 1 && data2[0] != 0u) || (asize2 == 0 && data2[0] == 0u));
    ::mp_limb_t tmp;
    if (sign1 == sign2) {
        // When the signs are identical, we can implement addition as a true addition.
        if (mppp_unlikely(limb_add_overflow(data1[0], data2[0], &tmp))) {
            return false;
        }
        // Assign the output. asize can be zero (sign1 == sign2 == 0) or 1.
        rop._mp_size = sign1;
        rdata[0] = tmp;
    } else {
        // When the signs differ, we need to implement addition as a subtraction.
        // NOTE: this also includes the case in which only one of the operands is zero.
        if (data1[0] >= data2[0]) {
            // op1 is not smaller than op2.
            tmp = data1[0] - data2[0];
            // asize is either 1 or 0 (0 iff abs(op1) == abs(op2)).
            rop._mp_size = sign1 * static_cast<int>(tmp != 0u);
            rdata[0] = tmp;
        } else {
            // NOTE: this has to be one, as data2[0] and data1[0] cannot be equal.
            rop._mp_size = sign2;
            rdata[0] = data2[0] - data1[0];
        }
    }
    return true;
}

// Optimization for two-limbs statics with no nails.
// Small helper to compare two statics of equal asize 1 or 2.
inline int integer_compare_limbs_2(const ::mp_limb_t *data1, const ::mp_limb_t *data2, mpz_size_t asize)
{
    // NOTE: this requires no nail bits.
    assert(!GMP_NAIL_BITS);
    assert(asize == 1 || asize == 2);
    // Start comparing from the top.
    auto cmp_idx = asize - 1;
    if (data1[cmp_idx] != data2[cmp_idx]) {
        return data1[cmp_idx] > data2[cmp_idx] ? 1 : -1;
    }
    // The top limbs are equal, move down one limb.
    // If we are already at the bottom limb, it means the two numbers are equal.
    if (!cmp_idx) {
        return 0;
    }
    --cmp_idx;
    if (data1[cmp_idx] != data2[cmp_idx]) {
        return data1[cmp_idx] > data2[cmp_idx] ? 1 : -1;
    }
    return 0;
}

template <std::size_t SSize>
inline bool static_add_impl(static_int<SSize> &rop, const static_int<SSize> &op1, const static_int<SSize> &op2,
                            mpz_size_t asize1, mpz_size_t asize2, int sign1, int sign2,
                            const std::integral_constant<int, 2> &)
{
    auto rdata = &rop.m_limbs[0];
    auto data1 = &op1.m_limbs[0], data2 = &op2.m_limbs[0];
    if (sign1 == sign2) {
        // NOTE: this handles the case in which the numbers have the same sign, including 0 + 0.
        //
        // NOTE: this is the implementation for 2 limbs, even if potentially the operands have 1 limb.
        // The idea here is that it's better to do a few computations more rather than paying the branching
        // cost. 1-limb operands will have the upper limb set to zero from the zero-initialization of
        // the limbs of static ints.
        //
        // NOTE: the rop hi limb might spill over either from the addition of the hi limbs
        // of op1 and op2, or by the addition of carry coming over from the addition of
        // the lo limbs of op1 and op2.
        //
        // Add the hi and lo limbs.
        const auto a = data1[0], b = data2[0], c = data1[1], d = data2[1];
        ::mp_limb_t lo, hi1, hi2;
        const ::mp_limb_t cy_lo = limb_add_overflow(a, b, &lo), cy_hi1 = limb_add_overflow(c, d, &hi1),
                          cy_hi2 = limb_add_overflow(hi1, cy_lo, &hi2);
        // The result will overflow if we have any carry relating to the high limb.
        if (mppp_unlikely(cy_hi1 || cy_hi2)) {
            return false;
        }
        // For the size compuation:
        // - if sign1 == 0, size is zero,
        // - if sign1 == +-1, then the size is either +-1 or +-2: asize is 2 if the result
        //   has a nonzero 2nd limb, otherwise asize is 1.
        rop._mp_size = sign1 * (static_cast<int>(hi2 != 0u) + 1);
        rdata[0] = lo;
        rdata[1] = hi2;
    } else {
        // When the signs differ, we need to implement addition as a subtraction.
        // NOTE: this also includes the case in which only one of the operands is zero.
        if (asize1 > asize2 || (asize1 == asize2 && integer_compare_limbs_2(data1, data2, asize1) >= 0)) {
            // op1 is >= op2 in absolute value.
            const auto lo = data1[0] - data2[0];
            // If there's a borrow, then hi1 > hi2, otherwise we would have a negative result.
            assert(data1[0] >= data2[0] || data1[1] > data2[1]);
            // This can never wrap around, at most it goes to zero.
            const auto hi = data1[1] - data2[1] - static_cast<::mp_limb_t>(data1[0] < data2[0]);
            // asize can be 0, 1 or 2.
            // NOTE: this contraption ensures the correct result. The possibilities for hi/lo
            // nonzero are:
            // hi | lo | asize
            // ---------------
            //  1 |  1 |     2
            //  1 |  0 |     2
            //  0 |  1 |     1
            //  0 |  0 |     0
            // The sign1 (which cannot be zero due to the branch we are in) takes care of the sign of the size.
            // Use '|' instead of '||' as we don't need to short circuit on this.
            rop._mp_size = sign1 * static_cast<int>((lo != 0u) | (hi != 0u)) * (static_cast<int>(hi != 0u) + 1);
            rdata[0] = lo;
            rdata[1] = hi;
        } else {
            // op2 is > op1 in absolute value.
            const auto lo = data2[0] - data1[0];
            assert(data2[0] >= data1[0] || data2[1] > data1[1]);
            const auto hi = data2[1] - data1[1] - static_cast<::mp_limb_t>(data2[0] < data1[0]);
            // asize can be 1 or 2, but not zero as we know abs(op1) != abs(op2). Same idea as above.
            rop._mp_size = sign2 * (static_cast<int>(hi != 0u) + 1);
            rdata[0] = lo;
            rdata[1] = hi;
        }
    }
    return true;
}

template <bool AddOrSub, std::size_t SSize>
inline bool static_addsub(static_int<SSize> &rop, const static_int<SSize> &op1, const static_int<SSize> &op2)
{
    // NOTE: effectively negate op2 if we are subtracting.
    mpz_size_t asize1 = op1._mp_size, asize2 = AddOrSub ? op2._mp_size : -op2._mp_size;
    int sign1 = asize1 != 0, sign2 = asize2 != 0;
    if (asize1 < 0) {
        asize1 = -asize1;
        sign1 = -1;
    }
    if (asize2 < 0) {
        asize2 = -asize2;
        sign2 = -1;
    }
    const bool retval
        = static_add_impl(rop, op1, op2, asize1, asize2, sign1, sign2, integer_static_add_algo<static_int<SSize>>{});
    if (integer_static_add_algo<static_int<SSize>>::value == 0 && retval) {
        // If we used the mpn functions and we actually wrote into rop, then
        // make sure we zero out the unused limbs.
        rop.zero_unused_limbs();
    }
    return retval;
}
}

/// Ternary addition.
/**
 * This function will set \p rop to <tt>op1 + op2</tt>.
 *
 * @param rop the return value.
 * @param op1 the first argument.
 * @param op2 the second argument.
 *
 * @return a reference to \p rop.
 */
template <std::size_t SSize>
inline integer<SSize> &add(integer<SSize> &rop, const integer<SSize> &op1, const integer<SSize> &op2)
{
    const bool s1 = op1.is_static(), s2 = op2.is_static();
    bool sr = rop.is_static();
    if (mppp_likely(s1 && s2)) {
        // If both op1 and op2 are static, we will try to do the statid add.
        // We might need to downgrade rop to static.
        if (!sr) {
            // NOTE: here we are sure rop is distinct from op1/op2, as
            // rop is dynamic and op1/op2 are both static.
            rop.set_zero();
            sr = true;
        }
        if (mppp_likely(
                static_addsub<true>(rop._get_union().g_st(), op1._get_union().g_st(), op2._get_union().g_st()))) {
            return rop;
        }
    }
    if (sr) {
        rop._get_union().promote(SSize + 1u);
    }
    ::mpz_add(&rop._get_union().g_dy(), op1.get_mpz_view(), op2.get_mpz_view());
    return rop;
}

inline namespace detail
{

// Metaprogramming for selecting the algorithm for static add/sub with ui. The selection happens via
// an std::integral_constant with 3 possible values:
// - 0 (default case): use the GMP mpn functions,
// - 1: selected when there are no nail bits and the static size is 1,
// - 2: selected when there are no nail bits and the static size is 2.
template <typename SInt>
using integer_static_addsub_ui_algo = std::integral_constant<
    int, (!GMP_NAIL_BITS && SInt::s_size == 1) ? 1 : ((!GMP_NAIL_BITS && SInt::s_size == 2) ? 2 : 0)>;

// mpn implementation.
template <bool AddOrSub, std::size_t SSize>
inline bool static_addsub_ui_impl(static_int<SSize> &rop, const static_int<SSize> &op1, mpz_size_t asize1, int sign1,
                                  unsigned long op2, const std::integral_constant<int, 0> &)
{
    auto rdata = &rop.m_limbs[0];
    auto data1 = &op1.m_limbs[0];
    const auto size1 = op1._mp_size;
    const auto l2 = static_cast<::mp_limb_t>(op2);
    // mpn functions require nonzero arguments.
    if (mppp_unlikely(!l2)) {
        rop._mp_size = size1;
        copy_limbs(data1, data1 + asize1, rdata);
        return true;
    }
    // We now know l2 is not zero, so its "sign" must be either 1 or -1.
    constexpr int sign2 = AddOrSub ? 1 : -1;
    if (mppp_unlikely(!sign1)) {
        // NOTE: this has to be +-1 because l2 == 0 is handled above.
        rop._mp_size = sign2;
        rdata[0] = l2;
        return true;
    }
    // This is the same overflow check as in static_addsub(). As in static_addsub(), this is not as tight/precise
    // as it could be.
    const bool c1 = std::size_t(asize1) == SSize && ((data1[asize1 - 1] & GMP_NUMB_MASK) >> (GMP_NUMB_BITS - 1));
    const bool c2 = SSize == 1u && (l2 >> (GMP_NUMB_BITS - 1));
    if (mppp_unlikely(c1 || c2)) {
        return false;
    }
    if (sign1 == sign2) {
        // op1 and op2 have the same sign. Implement as a true addition.
        if (::mpn_add_1(rdata, data1, static_cast<::mp_size_t>(asize1), l2)) {
            assert(asize1 < static_int<SSize>::s_size);
            // New size is old size + 1 if old size was positive, old size - 1 otherwise.
            rop._mp_size = size1 + sign2;
            // NOTE: there should be no need to use GMP_NUMB_MASK here.
            rdata[asize1] = 1u;
        } else {
            // Without carry, the size is unchanged.
            rop._mp_size = size1;
        }
    } else {
        // op1 and op2 have different signs. Implement as a subtraction.
        if (asize1 > 1 || (asize1 == 1 && (data1[0] & GMP_NUMB_MASK) >= l2)) {
            // abs(op1) >= abs(op2).
            const auto br = ::mpn_sub_1(rdata, data1, static_cast<::mp_size_t>(asize1), l2);
            (void)br;
            assert(!br);
            // The asize can be the original one or original - 1 (we subtracted a limb). If size1 was positive,
            // sign2 has to be negative and we potentially subtract 1, if size1 was negative then sign2 has to be
            // positive and we potentially add 1.
            rop._mp_size = size1 + sign2 * !(rdata[asize1 - 1] & GMP_NUMB_MASK);
        } else {
            // abs(op2) > abs(op1).
            const auto br = ::mpn_sub_1(rdata, &l2, 1, data1[0]);
            (void)br;
            assert(!br);
            // The size must be +-1, as abs(op2) == abs(op1) is handled above.
            assert((rdata[0] & GMP_NUMB_MASK));
            rop._mp_size = sign2;
        }
    }
    return true;
}

// 1-limb optimisation (no nails).
template <bool AddOrSub, std::size_t SSize>
inline bool static_addsub_ui_impl(static_int<SSize> &rop, const static_int<SSize> &op1, mpz_size_t, int sign1,
                                  unsigned long op2, const std::integral_constant<int, 1> &)
{
    const auto l1 = op1.m_limbs[0];
    const auto l2 = static_cast<::mp_limb_t>(op2);
    ::mp_limb_t tmp;
    if ((sign1 >= 0 && AddOrSub) || (sign1 <= 0 && !AddOrSub)) {
        // op1 non-negative and addition, or op1 non-positive and subtraction. Implement
        // as a true addition.
        if (mppp_unlikely(limb_add_overflow(l1, l2, &tmp))) {
            return false;
        }
        // The size is 1 for addition, -1 for subtraction, unless
        // the result is zero.
        rop._mp_size = (AddOrSub ? 1 : -1) * (tmp != 0u);
        rop.m_limbs[0] = tmp;
    } else {
        // op1 negative and addition, or op1 positive and subtraction. Implement
        // as a subtraction.
        if (l1 >= l2) {
            // op1 has larger or equal abs.
            tmp = l1 - l2;
            // asize is 1 or 0, 0 iff l1 == l2. Sign is negative for add, positive for sub.
            rop._mp_size = (AddOrSub ? -1 : 1) * (tmp != 0u);
            rop.m_limbs[0] = tmp;
        } else {
            // op1 has smaller abs. The result will be positive for add, negative
            // for sub (cannot be zero as it is handled above).
            rop._mp_size = AddOrSub ? 1 : -1;
            rop.m_limbs[0] = l2 - l1;
        }
    }
    return true;
}

// 2-limb optimisation (no nails).
template <bool AddOrSub, std::size_t SSize>
inline bool static_addsub_ui_impl(static_int<SSize> &rop, const static_int<SSize> &op1, mpz_size_t asize1, int sign1,
                                  unsigned long op2, const std::integral_constant<int, 2> &)
{
    auto rdata = &rop.m_limbs[0];
    auto data1 = &op1.m_limbs[0];
    const auto l2 = static_cast<::mp_limb_t>(op2);
    if ((sign1 >= 0 && AddOrSub) || (sign1 <= 0 && !AddOrSub)) {
        // op1 non-negative and addition, or op1 non-positive and subtraction. Implement
        // as a true addition.
        // These two limbs will contain the result.
        ::mp_limb_t lo, hi;
        // Add l2 to the low limb, placing the result in lo.
        const ::mp_limb_t cy_lo = limb_add_overflow(data1[0], l2, &lo);
        // Add the carry from the low addition to the high limb, placing the result in hi.
        const ::mp_limb_t cy_hi = limb_add_overflow(data1[1], cy_lo, &hi);
        if (mppp_unlikely(cy_hi)) {
            return false;
        }
        // Compute the new asize. It can be 0, 1 or 2.
        // NOTE: see the comments in the 2-limb specialisation for addition.
        rop._mp_size
            = (AddOrSub ? 1 : -1) * static_cast<int>((lo != 0u) | (hi != 0u)) * (static_cast<int>(hi != 0u) + 1);
        // Write out.
        rdata[0] = lo;
        rdata[1] = hi;
    } else {
        // op1 negative and addition, or op1 positive and subtraction. Implement
        // as a subtraction.
        // Compare their absolute values.
        if (asize1 == 2 || data1[0] >= l2) {
            // op1 is >= op2 in absolute value.
            const auto lo = data1[0] - l2;
            // Sub from hi the borrow.
            const auto hi = data1[1] - static_cast<::mp_limb_t>(data1[0] < l2);
            // The final asize can be 2, 1 or 0. Sign is negative for add, positive for sub.
            rop._mp_size
                = (AddOrSub ? -1 : 1) * (static_cast<int>((lo != 0u) | (hi != 0u)) * (static_cast<int>(hi != 0u) + 1));
            rdata[0] = lo;
            rdata[1] = hi;
        } else {
            // op2 > op1 in absolute value.
            // Size has to be +-1.
            rop._mp_size = AddOrSub ? 1 : -1;
            rdata[0] = l2 - data1[0];
            rdata[1] = 0u;
        }
    }
    return true;
}

template <bool AddOrSub, std::size_t SSize>
inline bool static_addsub_ui(static_int<SSize> &rop, const static_int<SSize> &op1, unsigned long op2)
{
    mpz_size_t asize1 = op1._mp_size;
    int sign1 = asize1 != 0;
    if (asize1 < 0) {
        asize1 = -asize1;
        sign1 = -1;
    }
    const bool retval = static_addsub_ui_impl<AddOrSub>(rop, op1, asize1, sign1, op2,
                                                        integer_static_addsub_ui_algo<static_int<SSize>>{});
    if (integer_static_addsub_ui_algo<static_int<SSize>>::value == 0 && retval) {
        // If we used the mpn functions and we actually wrote into rop, then
        // make sure we zero out the unused limbs.
        rop.zero_unused_limbs();
    }
    return retval;
}
}

/// Ternary addition with <tt>unsigned long</tt>.
/**
 * This function will set \p rop to <tt>op1 + op2</tt>.
 *
 * @param rop the return value.
 * @param op1 the first argument.
 * @param op2 the second argument.
 *
 * @return a reference to \p rop.
 */
template <std::size_t SSize>
inline integer<SSize> &add_ui(integer<SSize> &rop, const integer<SSize> &op1, unsigned long op2)
{
    // LCOV_EXCL_START
    if (std::numeric_limits<unsigned long>::max() > GMP_NUMB_MASK) {
        // For the optimised version below to kick in we need to be sure we can safely convert
        // unsigned long to an ::mp_limb_t, modulo nail bits. This because in the optimised version
        // we cast forcibly op2 to ::mp_limb_t. Otherwise, we just call add() after converting op2 to an
        // integer.
        // NOTE: it does not look like this can be hit on any modern setup.
        add(rop, op1, integer<SSize>{op2});
        return rop;
    }
    // LCOV_EXCL_STOP
    const bool s1 = op1.is_static();
    bool sr = rop.is_static();
    if (mppp_likely(s1)) {
        if (!sr) {
            rop.set_zero();
            sr = true;
        }
        if (mppp_likely(static_addsub_ui<true>(rop._get_union().g_st(), op1._get_union().g_st(), op2))) {
            return rop;
        }
    }
    if (sr) {
        rop._get_union().promote(SSize + 1u);
    }
    ::mpz_add_ui(&rop._get_union().g_dy(), op1.get_mpz_view(), op2);
    return rop;
}

/// Ternary subtraction with <tt>unsigned long</tt>.
/**
 * This function will set \p rop to <tt>op1 - op2</tt>.
 *
 * @param rop the return value.
 * @param op1 the first argument.
 * @param op2 the second argument.
 *
 * @return a reference to \p rop.
 */
template <std::size_t SSize>
inline integer<SSize> &sub_ui(integer<SSize> &rop, const integer<SSize> &op1, unsigned long op2)
{
    // LCOV_EXCL_START
    if (std::numeric_limits<unsigned long>::max() > GMP_NUMB_MASK) {
        sub(rop, op1, integer<SSize>{op2});
        return rop;
    }
    // LCOV_EXCL_STOP
    const bool s1 = op1.is_static();
    bool sr = rop.is_static();
    if (mppp_likely(s1)) {
        if (!sr) {
            rop.set_zero();
            sr = true;
        }
        if (mppp_likely(static_addsub_ui<false>(rop._get_union().g_st(), op1._get_union().g_st(), op2))) {
            return rop;
        }
    }
    if (sr) {
        rop._get_union().promote(SSize + 1u);
    }
    ::mpz_sub_ui(&rop._get_union().g_dy(), op1.get_mpz_view(), op2);
    return rop;
}

/// Ternary subtraction.
/**
 * This function will set \p rop to <tt>op1 - op2</tt>.
 *
 * @param rop the return value.
 * @param op1 the first argument.
 * @param op2 the second argument.
 *
 * @return a reference to \p rop.
 */
template <std::size_t SSize>
inline integer<SSize> &sub(integer<SSize> &rop, const integer<SSize> &op1, const integer<SSize> &op2)
{
    const bool s1 = op1.is_static(), s2 = op2.is_static();
    bool sr = rop.is_static();
    if (mppp_likely(s1 && s2)) {
        if (!sr) {
            rop.set_zero();
            sr = true;
        }
        if (mppp_likely(
                static_addsub<false>(rop._get_union().g_st(), op1._get_union().g_st(), op2._get_union().g_st()))) {
            return rop;
        }
    }
    if (sr) {
        rop._get_union().promote(SSize + 1u);
    }
    ::mpz_sub(&rop._get_union().g_dy(), op1.get_mpz_view(), op2.get_mpz_view());
    return rop;
}

inline namespace detail
{

// The double limb multiplication optimization is available in the following cases:
// - no nails, we are on a 64bit MSVC build, the limb type has exactly 64 bits and GMP_NUMB_BITS is 64,
// - no nails, we have a 128bit unsigned available, the limb type has exactly 64 bits and GMP_NUMB_BITS is 64,
// - no nails, the smallest 64 bit unsigned type has exactly 64 bits, the limb type has exactly 32 bits and
//   GMP_NUMB_BITS is 32.
// NOTE: here we are checking that GMP_NUMB_BITS is the same as the limits ::digits property, which is probably
// rather redundant on any conceivable architecture.
using integer_have_dlimb_mul = std::integral_constant<bool,
#if (defined(_MSC_VER) && defined(_WIN64) && (GMP_NUMB_BITS == 64)) || (defined(MPPP_UINT128) && (GMP_NUMB_BITS == 64))
                                                      !GMP_NAIL_BITS && std::numeric_limits<::mp_limb_t>::digits == 64
#elif GMP_NUMB_BITS == 32
                                                      !GMP_NAIL_BITS
                                                          && std::numeric_limits<std::uint_least64_t>::digits == 64
                                                          && std::numeric_limits<::mp_limb_t>::digits == 32
#else
                                                      false
#endif
                                                      >;

template <typename SInt>
using integer_static_mul_algo
    = std::integral_constant<int, (SInt::s_size == 1 && integer_have_dlimb_mul::value)
                                      ? 1
                                      : ((SInt::s_size == 2 && integer_have_dlimb_mul::value) ? 2 : 0)>;

// mpn implementation.
// NOTE: this function (and the other overloads) returns 0 in case of success, otherwise it returns a hint
// about the size in limbs of the result.
template <std::size_t SSize>
inline std::size_t static_mul_impl(static_int<SSize> &rop, const static_int<SSize> &op1, const static_int<SSize> &op2,
                                   mpz_size_t asize1, mpz_size_t asize2, int sign1, int sign2,
                                   const std::integral_constant<int, 0> &)
{
    // Handle zeroes.
    if (mppp_unlikely(!sign1 || !sign2)) {
        rop._mp_size = 0;
        return 0u;
    }
    auto rdata = &rop.m_limbs[0];
    auto data1 = &op1.m_limbs[0], data2 = &op2.m_limbs[0];
    const auto max_asize = std::size_t(asize1 + asize2);
    // Temporary storage, to be used if we cannot write into rop.
    std::array<::mp_limb_t, SSize * 2u> res;
    // We can write directly into rop if these conditions hold:
    // - rop does not overlap with op1 and op2,
    // - SSize is large enough to hold the max size of the result.
    ::mp_limb_t *MPPP_RESTRICT res_data = (rdata != data1 && rdata != data2 && max_asize <= SSize) ? rdata : res.data();
    // Proceed to the multiplication.
    ::mp_limb_t hi;
    if (asize2 == 1) {
        // NOTE: the 1-limb versions do not write the hi limb, we have to write it ourselves.
        hi = ::mpn_mul_1(res_data, data1, static_cast<::mp_size_t>(asize1), data2[0]);
        res_data[asize1] = hi;
    } else if (asize1 == 1) {
        hi = ::mpn_mul_1(res_data, data2, static_cast<::mp_size_t>(asize2), data1[0]);
        res_data[asize2] = hi;
    } else if (asize1 == asize2) {
        ::mpn_mul_n(res_data, data1, data2, static_cast<::mp_size_t>(asize1));
        hi = res_data[2 * asize1 - 1];
    } else if (asize1 >= asize2) {
        hi = ::mpn_mul(res_data, data1, static_cast<::mp_size_t>(asize1), data2, static_cast<::mp_size_t>(asize2));
    } else {
        hi = ::mpn_mul(res_data, data2, static_cast<::mp_size_t>(asize2), data1, static_cast<::mp_size_t>(asize1));
    }
    // The actual size.
    const std::size_t asize = max_asize - unsigned(hi == 0u);
    if (res_data == rdata) {
        // If we wrote directly into rop, it means that we had enough storage in it to begin with.
        rop._mp_size = mpz_size_t(asize);
        if (sign1 != sign2) {
            rop._mp_size = -rop._mp_size;
        }
        return 0u;
    }
    // If we used the temporary storage, we need to check if we can write into rop.
    if (asize > SSize) {
        // Not enough space, return the size.
        return asize;
    }
    // Enough space, set size and copy limbs.
    rop._mp_size = mpz_size_t(asize);
    if (sign1 != sign2) {
        rop._mp_size = -rop._mp_size;
    }
    // NOTE: here we are sure that res_data != rdata, as we checked it earlier.
    copy_limbs_no(res_data, res_data + asize, rdata);
    return 0u;
}

#if defined(_MSC_VER) && defined(_WIN64) && (GMP_NUMB_BITS == 64) && !GMP_NAIL_BITS
inline ::mp_limb_t dlimb_mul(::mp_limb_t op1, ::mp_limb_t op2, ::mp_limb_t *hi)
{
    return ::UnsignedMultiply128(op1, op2, hi);
}
#elif defined(MPPP_UINT128) && (GMP_NUMB_BITS == 64) && !GMP_NAIL_BITS
inline ::mp_limb_t dlimb_mul(::mp_limb_t op1, ::mp_limb_t op2, ::mp_limb_t *hi)
{
    using dlimb_t = MPPP_UINT128;
    const dlimb_t res = dlimb_t(op1) * op2;
    *hi = static_cast<::mp_limb_t>(res >> 64);
    return static_cast<::mp_limb_t>(res);
}
#elif GMP_NUMB_BITS == 32 && !GMP_NAIL_BITS
inline ::mp_limb_t dlimb_mul(::mp_limb_t op1, ::mp_limb_t op2, ::mp_limb_t *hi)
{
    using dlimb_t = std::uint_least64_t;
    const dlimb_t res = dlimb_t(op1) * op2;
    *hi = static_cast<::mp_limb_t>(res >> 32);
    return static_cast<::mp_limb_t>(res);
}
#endif

// 1-limb optimization via dlimb.
template <std::size_t SSize>
inline std::size_t static_mul_impl(static_int<SSize> &rop, const static_int<SSize> &op1, const static_int<SSize> &op2,
                                   mpz_size_t, mpz_size_t, int, int, const std::integral_constant<int, 1> &)
{
    ::mp_limb_t hi;
    const ::mp_limb_t lo = dlimb_mul(op1.m_limbs[0], op2.m_limbs[0], &hi);
    if (mppp_unlikely(hi)) {
        return 2u;
    }
    // The size will be zero if at least one operand is zero, otherwise +-1
    // depending on the signs of the operands.
    rop._mp_size = op1._mp_size * op2._mp_size;
    rop.m_limbs[0] = lo;
    return 0u;
}

// 2-limb optimization via dlimb.
template <std::size_t SSize>
inline std::size_t static_mul_impl(static_int<SSize> &rop, const static_int<SSize> &op1, const static_int<SSize> &op2,
                                   mpz_size_t asize1, mpz_size_t asize2, int sign1, int sign2,
                                   const std::integral_constant<int, 2> &)
{
    if (asize1 <= 1 && asize2 <= 1) {
        // NOTE: this handles zeroes as well: we know that the limbs are all zeroes in such
        // a case (as we always guarantee unused limbs are zeroed), and the sign1 * sign2 multiplication takes care of
        // the size.
        rop.m_limbs[0] = dlimb_mul(op1.m_limbs[0], op2.m_limbs[0], &rop.m_limbs[1]);
        rop._mp_size = sign1 * sign2 * static_cast<mpz_size_t>(2 - (rop.m_limbs[1] == 0u));
        return 0u;
    }
    if (asize1 != asize2) {
        // The only possibility of success is 2limbs x 1limb.
        //
        //             b      a X
        //                    c
        // --------------------
        // tmp[2] tmp[1] tmp[0]
        //
        ::mp_limb_t a = op1.m_limbs[0], b = op1.m_limbs[1], c = op2.m_limbs[0];
        if (asize1 < asize2) {
            // Switch them around if needed.
            a = op2.m_limbs[0], b = op2.m_limbs[1], c = op1.m_limbs[0];
        }
        ::mp_limb_t ca_lo, ca_hi, cb_lo, cb_hi, tmp0, tmp1, tmp2;
        ca_lo = dlimb_mul(c, a, &ca_hi);
        cb_lo = dlimb_mul(c, b, &cb_hi);
        tmp0 = ca_lo;
        const auto cy = limb_add_overflow(cb_lo, ca_hi, &tmp1);
        tmp2 = cb_hi + cy;
        // Now determine the size. asize must be at least 2.
        const mpz_size_t asize = 2 + mpz_size_t(tmp2 != 0u);
        if (asize == 2) {
            // Size is good, write out the result.
            rop._mp_size = sign1 * sign2 * asize;
            rop.m_limbs[0] = tmp0;
            rop.m_limbs[1] = tmp1;
            return 0u;
        }
    }
    // Return 4 as a size hint. Real size could be 3, but GMP will require 4 limbs
    // of storage to perform the operation anyway.
    return 4u;
}

template <std::size_t SSize>
inline std::size_t static_mul(static_int<SSize> &rop, const static_int<SSize> &op1, const static_int<SSize> &op2)
{
    // Cache a few quantities, detect signs.
    mpz_size_t asize1 = op1._mp_size, asize2 = op2._mp_size;
    int sign1 = asize1 != 0, sign2 = asize2 != 0;
    if (asize1 < 0) {
        asize1 = -asize1;
        sign1 = -1;
    }
    if (asize2 < 0) {
        asize2 = -asize2;
        sign2 = -1;
    }
    const std::size_t retval
        = static_mul_impl(rop, op1, op2, asize1, asize2, sign1, sign2, integer_static_mul_algo<static_int<SSize>>{});
    if (integer_static_mul_algo<static_int<SSize>>::value == 0 && retval == 0u) {
        rop.zero_unused_limbs();
    }
    return retval;
}
}

/// Ternary multiplication.
/**
 * This function will set \p rop to <tt>op1 * op2</tt>.
 *
 * @param rop the return value.
 * @param op1 the first argument.
 * @param op2 the second argument.
 *
 * @return a reference to \p rop.
 */
template <std::size_t SSize>
inline integer<SSize> &mul(integer<SSize> &rop, const integer<SSize> &op1, const integer<SSize> &op2)
{
    const bool s1 = op1.is_static(), s2 = op2.is_static();
    bool sr = rop.is_static();
    std::size_t size_hint = 0u;
    if (mppp_likely(s1 && s2)) {
        if (!sr) {
            rop.set_zero();
            sr = true;
        }
        size_hint = static_mul(rop._get_union().g_st(), op1._get_union().g_st(), op2._get_union().g_st());
        if (mppp_likely(size_hint == 0u)) {
            return rop;
        }
    }
    if (sr) {
        // We use the size hint from the static_mul if available, otherwise a normal promotion will take place.
        // NOTE: here the best way of proceeding would be to calculate the max size of the result based on
        // the op1/op2 sizes, but for whatever reason this computation has disastrous performance consequences
        // on micro-benchmarks. We need to understand if that's the case in real-world scenarios as well, and
        // revisit this.
        rop._get_union().promote(size_hint);
    }
    ::mpz_mul(&rop._get_union().g_dy(), op1.get_mpz_view(), op2.get_mpz_view());
    return rop;
}

inline namespace detail
{

// Selection of the algorithm for addmul: if optimised algorithms exist for both add and mul, then use the
// optimised addmul algos. Otherwise, use the mpn one.
template <typename SInt>
using integer_static_addmul_algo = std::integral_constant<
    int, (integer_static_add_algo<SInt>::value == 2 && integer_static_mul_algo<SInt>::value == 2)
             ? 2
             : ((integer_static_add_algo<SInt>::value == 1 && integer_static_mul_algo<SInt>::value == 1) ? 1 : 0)>;

// NOTE: same return value as mul: 0 for success, otherwise a hint for the size of the result.
template <std::size_t SSize>
inline std::size_t static_addmul_impl(static_int<SSize> &rop, const static_int<SSize> &op1,
                                      const static_int<SSize> &op2, mpz_size_t asizer, mpz_size_t asize1,
                                      mpz_size_t asize2, int signr, int sign1, int sign2,
                                      const std::integral_constant<int, 0> &)
{
    // First try to do the static prod, if it does not work it's a failure.
    static_int<SSize> prod;
    if (mppp_unlikely(
            static_mul_impl(prod, op1, op2, asize1, asize2, sign1, sign2, std::integral_constant<int, 0>{}))) {
        // This is the maximum size a static addmul can have.
        return SSize * 2u + 1u;
    }
    // Determine sign and asize of the product.
    mpz_size_t asize_prod = prod._mp_size;
    int sign_prod = (asize_prod != 0);
    if (asize_prod < 0) {
        asize_prod = -asize_prod;
        sign_prod = -1;
    }
    // Try to do the add.
    if (mppp_unlikely(
            !static_add_impl(rop, rop, prod, asizer, asize_prod, signr, sign_prod, std::integral_constant<int, 0>{}))) {
        return SSize + 1u;
    }
    return 0u;
}

template <std::size_t SSize>
inline std::size_t static_addmul_impl(static_int<SSize> &rop, const static_int<SSize> &op1,
                                      const static_int<SSize> &op2, mpz_size_t, mpz_size_t, mpz_size_t, int signr,
                                      int sign1, int sign2, const std::integral_constant<int, 1> &)
{
    // First we do op1 * op2.
    ::mp_limb_t tmp;
    const ::mp_limb_t prod = dlimb_mul(op1.m_limbs[0], op2.m_limbs[0], &tmp);
    if (mppp_unlikely(tmp)) {
        return 3u;
    }
    // Determine the sign of the product: 0, 1 or -1.
    const int sign_prod = sign1 * sign2;
    // Now add/sub.
    if (signr == sign_prod) {
        // Same sign, do addition with overflow check.
        if (mppp_unlikely(limb_add_overflow(rop.m_limbs[0], prod, &tmp))) {
            return 2u;
        }
        // Assign the output.
        rop._mp_size = signr;
        rop.m_limbs[0] = tmp;
    } else {
        // When the signs differ, we need to implement addition as a subtraction.
        if (rop.m_limbs[0] >= prod) {
            // abs(rop) >= abs(prod).
            tmp = rop.m_limbs[0] - prod;
            // asize is either 1 or 0 (0 iff rop == prod).
            rop._mp_size = signr * static_cast<int>(tmp != 0u);
            rop.m_limbs[0] = tmp;
        } else {
            // NOTE: this cannot be zero, as rop and prod cannot be equal.
            rop._mp_size = sign_prod;
            rop.m_limbs[0] = prod - rop.m_limbs[0];
        }
    }
    return 0u;
}

template <std::size_t SSize>
inline std::size_t static_addmul_impl(static_int<SSize> &rop, const static_int<SSize> &op1,
                                      const static_int<SSize> &op2, mpz_size_t asizer, mpz_size_t asize1,
                                      mpz_size_t asize2, int signr, int sign1, int sign2,
                                      const std::integral_constant<int, 2> &)
{
    if (mppp_unlikely(!asize1 || !asize2)) {
        // If op1 or op2 are zero, rop will be unchanged.
        return 0u;
    }
    // Handle op1 * op2.
    std::array<::mp_limb_t, 2> prod;
    const int sign_prod = sign1 * sign2;
    mpz_size_t asize_prod;
    if (asize1 == 1 && asize2 == 1) {
        prod[0] = dlimb_mul(op1.m_limbs[0], op2.m_limbs[0], &prod[1]);
        asize_prod = (asize1 + asize2) - (prod[1] == 0u);
    } else {
        // The only possibility of success is 2limbs x 1limb.
        //
        //       b       a X
        //               c
        // ---------------
        // prod[1] prod[0]
        //
        if (mppp_unlikely(asize1 == asize2)) {
            // This means that both sizes are 2.
            return 5u;
        }
        ::mp_limb_t a = op1.m_limbs[0], b = op1.m_limbs[1], c = op2.m_limbs[0];
        if (asize1 < asize2) {
            // Switch them around if needed.
            a = op2.m_limbs[0], b = op2.m_limbs[1], c = op1.m_limbs[0];
        }
        ::mp_limb_t ca_hi, cb_hi;
        // These are the three operations that build the result, two mults, one add.
        prod[0] = dlimb_mul(c, a, &ca_hi);
        prod[1] = dlimb_mul(c, b, &cb_hi);
        // NOTE: we can use this with overlapping arguments because the first two
        // are passed as copies.
        const auto cy = limb_add_overflow(prod[1], ca_hi, &prod[1]);
        // Check if the third limb exists, if so we return failure.
        if (mppp_unlikely(cb_hi || cy)) {
            return 4u;
        }
        asize_prod = 2;
    }
    // Proceed to the addition.
    if (signr == sign_prod) {
        // Add the hi and lo limbs.
        ::mp_limb_t lo, hi1, hi2;
        const ::mp_limb_t cy_lo = limb_add_overflow(rop.m_limbs[0], prod[0], &lo),
                          cy_hi1 = limb_add_overflow(rop.m_limbs[1], prod[1], &hi1),
                          cy_hi2 = limb_add_overflow(hi1, cy_lo, &hi2);
        // The result will overflow if we have any carry relating to the high limb.
        if (mppp_unlikely(cy_hi1 || cy_hi2)) {
            return 3u;
        }
        // For the size compuation:
        // - cannot be zero, as prod is not zero,
        // - if signr == +-1, then the size is either +-1 or +-2: asize is 2 if the result
        //   has a nonzero 2nd limb, otherwise asize is 1.
        rop._mp_size = signr * (static_cast<int>(hi2 != 0u) + 1);
        rop.m_limbs[0] = lo;
        rop.m_limbs[1] = hi2;
    } else {
        // When the signs differ, we need to implement addition as a subtraction.
        if (asizer > asize_prod
            || (asizer == asize_prod && integer_compare_limbs_2(&rop.m_limbs[0], &prod[0], asizer) >= 0)) {
            // rop >= prod in absolute value.
            const auto lo = rop.m_limbs[0] - prod[0];
            // If there's a borrow, then hi1 > hi2, otherwise we would have a negative result.
            assert(rop.m_limbs[0] >= prod[0] || rop.m_limbs[1] > prod[1]);
            // This can never wrap around, at most it goes to zero.
            const auto hi = rop.m_limbs[1] - prod[1] - static_cast<::mp_limb_t>(rop.m_limbs[0] < prod[0]);
            // asize can be 0, 1 or 2.
            // NOTE: see the comments in the 2-limb specialisation for addition.
            rop._mp_size = signr * static_cast<int>((lo != 0u) | (hi != 0u)) * (static_cast<int>(hi != 0u) + 1);
            rop.m_limbs[0] = lo;
            rop.m_limbs[1] = hi;
        } else {
            // prod > rop in absolute value.
            const auto lo = prod[0] - rop.m_limbs[0];
            assert(prod[0] >= rop.m_limbs[0] || prod[1] > rop.m_limbs[1]);
            const auto hi = prod[1] - rop.m_limbs[1] - static_cast<::mp_limb_t>(prod[0] < rop.m_limbs[0]);
            // asize can be 1 or 2, but not zero as we know abs(prod) != abs(rop).
            rop._mp_size = sign_prod * (static_cast<int>(hi != 0u) + 1);
            rop.m_limbs[0] = lo;
            rop.m_limbs[1] = hi;
        }
    }
    return 0u;
}

template <bool AddOrSub, std::size_t SSize>
inline std::size_t static_addsubmul(static_int<SSize> &rop, const static_int<SSize> &op1, const static_int<SSize> &op2)
{
    // NOTE: negate op2 in case we are doing a submul.
    mpz_size_t asizer = rop._mp_size, asize1 = op1._mp_size, asize2 = AddOrSub ? op2._mp_size : -op2._mp_size;
    int signr = asizer != 0, sign1 = asize1 != 0, sign2 = asize2 != 0;
    if (asizer < 0) {
        asizer = -asizer;
        signr = -1;
    }
    if (asize1 < 0) {
        asize1 = -asize1;
        sign1 = -1;
    }
    if (asize2 < 0) {
        asize2 = -asize2;
        sign2 = -1;
    }
    const std::size_t retval = static_addmul_impl(rop, op1, op2, asizer, asize1, asize2, signr, sign1, sign2,
                                                  integer_static_addmul_algo<static_int<SSize>>{});
    if (integer_static_addmul_algo<static_int<SSize>>::value == 0 && retval == 0u) {
        rop.zero_unused_limbs();
    }
    return retval;
}
}

/// Ternary multiply–add.
/**
 * This function will set \p rop to <tt>rop + op1 * op2</tt>.
 *
 * @param rop the return value.
 * @param op1 the first argument.
 * @param op2 the second argument.
 *
 * @return a reference to \p rop.
 */
template <std::size_t SSize>
inline integer<SSize> &addmul(integer<SSize> &rop, const integer<SSize> &op1, const integer<SSize> &op2)
{
    const bool sr = rop.is_static(), s1 = op1.is_static(), s2 = op2.is_static();
    std::size_t size_hint = 0u;
    if (mppp_likely(sr && s1 && s2)) {
        size_hint = static_addsubmul<true>(rop._get_union().g_st(), op1._get_union().g_st(), op2._get_union().g_st());
        if (mppp_likely(size_hint == 0u)) {
            return rop;
        }
    }
    if (sr) {
        rop._get_union().promote(size_hint);
    }
    ::mpz_addmul(&rop._get_union().g_dy(), op1.get_mpz_view(), op2.get_mpz_view());
    return rop;
}

/// Ternary multiply–sub.
/**
 * This function will set \p rop to <tt>rop - op1 * op2</tt>.
 *
 * @param rop the return value.
 * @param op1 the first argument.
 * @param op2 the second argument.
 *
 * @return a reference to \p rop.
 */
template <std::size_t SSize>
inline integer<SSize> &submul(integer<SSize> &rop, const integer<SSize> &op1, const integer<SSize> &op2)
{
    const bool sr = rop.is_static(), s1 = op1.is_static(), s2 = op2.is_static();
    std::size_t size_hint = 0u;
    if (mppp_likely(sr && s1 && s2)) {
        size_hint = static_addsubmul<false>(rop._get_union().g_st(), op1._get_union().g_st(), op2._get_union().g_st());
        if (mppp_likely(size_hint == 0u)) {
            return rop;
        }
    }
    if (sr) {
        rop._get_union().promote(size_hint);
    }
    ::mpz_submul(&rop._get_union().g_dy(), op1.get_mpz_view(), op2.get_mpz_view());
    return rop;
}

inline namespace detail
{

// Selection of the algorithm for static mul_2exp.
template <typename SInt>
using integer_static_mul_2exp_algo = std::integral_constant<int, (SInt::s_size > 2) ? 0 : SInt::s_size>;

// mpn implementation.
template <std::size_t SSize>
inline std::size_t static_mul_2exp_impl(static_int<SSize> &rop, const static_int<SSize> &n, std::size_t s,
                                        const std::integral_constant<int, 0> &)
{
    mpz_size_t asize = n._mp_size;
    if (s == 0u || asize == 0) {
        // If shift is zero, or the operand is zero, write n into rop and return success.
        rop = n;
        return 0u;
    }
    // Finish setting up asize and sign.
    int sign = 1;
    if (asize < 0) {
        asize = -asize;
        sign = -1;
    }
    // ls: number of entire limbs shifted.
    // rs: effective shift that will be passed to the mpn function.
    const std::size_t ls = s / unsigned(GMP_NUMB_BITS), rs = s % unsigned(GMP_NUMB_BITS);
    // At the very minimum, the new asize will be the old asize plus ls.
    // Check if we can represent it first.
    // NOTE: use >= because we may need to increase by 1 new_asize at the end, as a size hint.
    // LCOV_EXCL_START
    if (mppp_unlikely(ls >= std::numeric_limits<std::size_t>::max() - static_cast<std::size_t>(asize))) {
        // NOTE: don't think this can be hit on any setup currently.
        throw std::overflow_error("A left bitshift value of " + std::to_string(s) + " is too large");
    }
    // LCOV_EXCL_STOP
    const std::size_t new_asize = static_cast<std::size_t>(asize) + ls;
    if (new_asize < SSize) {
        // In this case the operation will always succeed, and we can write directly into rop.
        ::mp_limb_t ret = 0u;
        if (rs) {
            // Perform the shift via the mpn function, if we are effectively shifting at least 1 bit.
            // Overlapping is fine, as it is guaranteed that rop.m_limbs.data() + ls >= n.m_limbs.data().
            ret = ::mpn_lshift(rop.m_limbs.data() + ls, n.m_limbs.data(), static_cast<::mp_size_t>(asize),
                               unsigned(rs));
            // Write bits spilling out.
            rop.m_limbs[new_asize] = ret;
        } else {
            // Otherwise, just copy over (the mpn function requires the shift to be at least 1).
            // NOTE: we have to use move_backward here because the ranges may be overlapping but not
            // starting at the same pointer (in which case we could've used copy_limbs()). Here we know ls is not
            // zero: we don't have a remainder, and s == 0 was already handled above. Hence, new_asize > asize.
            assert(new_asize > static_cast<std::size_t>(asize));
            std::move_backward(n.m_limbs.begin(), n.m_limbs.begin() + asize, rop.m_limbs.begin() + new_asize);
        }
        // Zero the lower limbs vacated by the shift. We need to do this as we don't know
        // the state of rop.
        std::fill(rop.m_limbs.data(), rop.m_limbs.data() + ls, ::mp_limb_t(0));
        // Set the size.
        rop._mp_size = sign * (static_cast<mpz_size_t>(new_asize) + (ret != 0u));
        return 0u;
    }
    if (new_asize == SSize) {
        if (rs) {
            // In this case the operation may fail, so we need to write to temporary storage.
            std::array<::mp_limb_t, SSize> tmp;
            if (::mpn_lshift(tmp.data(), n.m_limbs.data(), static_cast<::mp_size_t>(asize), unsigned(rs))) {
                return SSize + 1u;
            }
            // The shift was successful without spill over, copy the content from the tmp
            // storage into rop.
            copy_limbs_no(tmp.data(), tmp.data() + asize, rop.m_limbs.data() + ls);
        } else {
            // If we shifted by a multiple of the limb size, then we can write directly to rop.
            assert(new_asize > static_cast<std::size_t>(asize));
            std::move_backward(n.m_limbs.begin(), n.m_limbs.begin() + asize, rop.m_limbs.begin() + new_asize);
        }
        // Zero the lower limbs vacated by the shift.
        std::fill(rop.m_limbs.data(), rop.m_limbs.data() + ls, ::mp_limb_t(0));
        // Set the size.
        rop._mp_size = sign * static_cast<mpz_size_t>(new_asize);
        return 0u;
    }
    // Shift is too much, the size will overflow the static limit. Return a hint for the size of the result.
    // We checked above we can compute this safely.
    return new_asize + 1u;
}

// 1-limb optimisation.
template <std::size_t SSize>
inline std::size_t static_mul_2exp_impl(static_int<SSize> &rop, const static_int<SSize> &n, std::size_t s,
                                        const std::integral_constant<int, 1> &)
{
    const ::mp_limb_t l = n.m_limbs[0] & GMP_NUMB_MASK;
    if (s == 0u || l == 0u) {
        // If shift is zero, or the operand is zero, write n into rop and return success.
        rop = n;
        return 0u;
    }
    if (mppp_unlikely(s >= unsigned(GMP_NUMB_BITS) || (l >> (unsigned(GMP_NUMB_BITS) - s)))) {
        // The two conditions:
        // - if the shift is >= number of data bits, the operation will certainly
        //   fail (as the operand is nonzero);
        // - shifting by s would overflow  the single limb.
        // NOTE: s is at least 1, so in the right shift above we never risk UB due to too much shift.
        // NOTE: for the size hint: s / nbits is the number of entire limbs shifted, +1 because the shifted
        // limbs add to the current size (1), +1 because another limb might be needed.
        // NOTE: paranoia static assert to make sure there's no chance of overflowing.
        static_assert(GMP_NUMB_BITS > 1, "Invalid number of bits.");
        return s / unsigned(GMP_NUMB_BITS) + 2u;
    }
    // Write out.
    rop.m_limbs[0] = l << s;
    // Size is the same as the input value.
    rop._mp_size = n._mp_size;
    return 0u;
}

// 2-limb optimisation.
template <std::size_t SSize>
inline std::size_t static_mul_2exp_impl(static_int<SSize> &rop, const static_int<SSize> &n, std::size_t s,
                                        const std::integral_constant<int, 2> &)
{
    mpz_size_t asize = n._mp_size;
    if (s == 0u || asize == 0) {
        rop = n;
        return 0u;
    }
    int sign = 1;
    if (asize < 0) {
        asize = -asize;
        sign = -1;
    }
    // Too much shift, this can never work on a nonzero value.
    static_assert(unsigned(GMP_NUMB_BITS) <= std::numeric_limits<unsigned>::max() / 2u, "Overflow error.");
    if (mppp_unlikely(s >= 2u * unsigned(GMP_NUMB_BITS))) {
        // NOTE: this is the generic formula to estimate the final size.
        // NOTE: paranoia static assert to make sure there's no chance of overflowing.
        static_assert(GMP_NUMB_BITS > 1, "Invalid number of bits.");
        // NOTE: asize is 2 at most, no need to check for overflow.
        return s / unsigned(GMP_NUMB_BITS) + 1u + static_cast<std::size_t>(asize);
    }
    if (s == unsigned(GMP_NUMB_BITS)) {
        // This case can be dealt with moving lo into hi, but only if asize is 1.
        if (mppp_unlikely(asize == 2)) {
            // asize is 2, shift is too much.
            return 3u;
        }
        rop.m_limbs[1u] = n.m_limbs[0u];
        rop.m_limbs[0u] = 0u;
        // The asize has to be 2.
        rop._mp_size = 2 * sign;
        return 0u;
    }
    // Temp hi lo limbs to store the result that will eventually go into rop.
    ::mp_limb_t lo = n.m_limbs[0u], hi = n.m_limbs[1u];
    if (s > unsigned(GMP_NUMB_BITS)) {
        if (mppp_unlikely(asize == 2)) {
            return s / unsigned(GMP_NUMB_BITS) + 1u + static_cast<std::size_t>(asize);
        }
        // Move lo to hi and set lo to zero.
        hi = n.m_limbs[0u];
        lo = 0u;
        // Update the shift.
        s = static_cast<std::size_t>(s - unsigned(GMP_NUMB_BITS));
    }
    // Check that hi will not be shifted too much. Note that
    // here and below s can never be zero, so we never shift too much.
    assert(s > 0u && s < unsigned(GMP_NUMB_BITS));
    if (mppp_unlikely((hi & GMP_NUMB_MASK) >> (unsigned(GMP_NUMB_BITS) - s))) {
        return 3u;
    }
    // Shift hi and lo. hi gets the carry over from lo.
    hi = ((hi & GMP_NUMB_MASK) << s) + ((lo & GMP_NUMB_MASK) >> (unsigned(GMP_NUMB_BITS) - s));
    // NOTE: here the result needs to be masked as well as the shift could
    // end up writing in nail bits.
    lo = ((lo & GMP_NUMB_MASK) << s) & GMP_NUMB_MASK;
    // Write rop.
    rop.m_limbs[0u] = lo;
    rop.m_limbs[1u] = hi;
    // asize is at least 1.
    rop._mp_size = sign * (1 + (hi != 0u));
    return 0u;
}

template <std::size_t SSize>
inline std::size_t static_mul_2exp(static_int<SSize> &rop, const static_int<SSize> &n, std::size_t s)
{
    const std::size_t retval = static_mul_2exp_impl(rop, n, s, integer_static_mul_2exp_algo<static_int<SSize>>{});
    if (integer_static_mul_2exp_algo<static_int<SSize>>::value == 0 && retval == 0u) {
        rop.zero_unused_limbs();
    }
    return retval;
}
}

/// Ternary left shift.
/**
 * This function will set \p rop to \p n multiplied by <tt>2**s</tt>.
 *
 * @param rop the return value.
 * @param n the multiplicand.
 * @param s the bit shift value.
 *
 * @return a reference to \p rop.
 *
 * @throws std::overflow_error if \p s is larger than an implementation-defined limit.
 */
template <std::size_t SSize>
inline integer<SSize> &mul_2exp(integer<SSize> &rop, const integer<SSize> &n, ::mp_bitcnt_t s)
{
    const bool sn = n.is_static();
    bool sr = rop.is_static();
    std::size_t size_hint = 0u;
    if (mppp_likely(sn)) {
        // NOTE: we cast to size_t because it's more convenient for reasoning about number of limbs
        // in the implementation functions.
        // NOTE: do it before touching rop, for exception safety.
        const auto s_size = safe_cast<std::size_t>(s);
        if (!sr) {
            rop.set_zero();
            sr = true;
        }
        size_hint = static_mul_2exp(rop._get_union().g_st(), n._get_union().g_st(), s_size);
        if (mppp_likely(size_hint == 0u)) {
            return rop;
        }
    }
    if (sr) {
        rop._get_union().promote(size_hint);
    }
    ::mpz_mul_2exp(&rop._get_union().g_dy(), n.get_mpz_view(), s);
    return rop;
}

/// Binary negation.
/**
 * This method will set \p rop to <tt>-n</tt>.
 *
 * @param rop the return value.
 * @param n the integer that will be negated.
 *
 * @return a reference to \p rop.
 */
template <std::size_t SSize>
inline integer<SSize> &neg(integer<SSize> &rop, const integer<SSize> &n)
{
    rop = n;
    return rop.neg();
}

/// Unary negation.
/**
 * @param n the integer that will be negated.
 *
 * @return <tt>-n</tt>.
 */
template <std::size_t SSize>
inline integer<SSize> neg(const integer<SSize> &n)
{
    integer<SSize> ret(n);
    ret.neg();
    return ret;
}

/// Binary absolute value.
/**
 * This function will set \p rop to the absolute value of \p n.
 *
 * @param rop the return value.
 * @param n the argument.
 *
 * @return a reference to \p rop.
 */
template <std::size_t SSize>
inline integer<SSize> &abs(integer<SSize> &rop, const integer<SSize> &n)
{
    rop = n;
    return rop.abs();
}

/// Unary absolute value.
/**
 * @param n the argument.
 *
 * @return the absolute value of \p n.
 */
template <std::size_t SSize>
inline integer<SSize> abs(const integer<SSize> &n)
{
    integer<SSize> ret(n);
    ret.abs();
    return ret;
}

/** @} */

/** @defgroup integer_division integer_division
 *  @{
 */

inline namespace detail
{

// Detect the presence of dual-limb division. This is currently possible only if:
// - we are on a 32bit build (with the usual constraints that the types have exactly 32/64 bits and no nails),
// - we are on a 64bit build and we have the 128bit int type available (plus usual constraints).
// MSVC currently does not provide any primitive for 128bit division.
using integer_have_dlimb_div = std::integral_constant<bool,
#if defined(MPPP_UINT128) && (GMP_NUMB_BITS == 64)
                                                      !GMP_NAIL_BITS && std::numeric_limits<::mp_limb_t>::digits == 64
#elif GMP_NUMB_BITS == 32
                                                      !GMP_NAIL_BITS
                                                          && std::numeric_limits<std::uint_least64_t>::digits == 64
                                                          && std::numeric_limits<::mp_limb_t>::digits == 32
#else
                                                      false
#endif
                                                      >;

// Selection of algorithm for static division:
// - for 1 limb, we can always do static division,
// - for 2 limbs, we need the dual limb division if avaiable,
// - otherwise we just use the mpn functions.
template <typename SInt>
using integer_static_div_algo
    = std::integral_constant<int,
                             SInt::s_size == 1 ? 1 : ((SInt::s_size == 2 && integer_have_dlimb_div::value) ? 2 : 0)>;

// mpn implementation.
template <std::size_t SSize>
inline void static_div_impl(static_int<SSize> &q, static_int<SSize> &r, const static_int<SSize> &op1,
                            const static_int<SSize> &op2, mpz_size_t asize1, mpz_size_t asize2, int sign1, int sign2,
                            const std::integral_constant<int, 0> &)
{
    // First we check if the divisor is larger than the dividend (in abs limb size), as the mpn function
    // requires asize1 >= asize2.
    if (asize2 > asize1) {
        // Copy op1 into the remainder.
        r = op1;
        // Zero out q.
        q._mp_size = 0;
        return;
    }
    // We need to take care of potentially overlapping arguments. We know that q and r are distinct, but op1
    // could overlap with q or r, and op2 could overlap with op1, q or r.
    std::array<::mp_limb_t, SSize> op1_alt, op2_alt;
    const ::mp_limb_t *data1 = op1.m_limbs.data();
    const ::mp_limb_t *data2 = op2.m_limbs.data();
    if (&op1 == &q || &op1 == &r) {
        copy_limbs_no(data1, data1 + asize1, op1_alt.data());
        data1 = op1_alt.data();
    }
    if (&op2 == &q || &op2 == &r || &op1 == &op2) {
        copy_limbs_no(data2, data2 + asize2, op2_alt.data());
        data2 = op2_alt.data();
    }
    // Small helper function to verify that all pointers are distinct. Used exclusively for debugging purposes.
    auto distinct_op = [&q, &r, data1, data2]() -> bool {
        const ::mp_limb_t *ptrs[] = {q.m_limbs.data(), r.m_limbs.data(), data1, data2};
        std::sort(ptrs, ptrs + 4, std::less<const ::mp_limb_t *>());
        return std::unique(ptrs, ptrs + 4) == (ptrs + 4);
    };
    (void)distinct_op;
    assert(distinct_op());
    // Proceed to the division.
    if (asize2 == 1) {
        // Optimization when the divisor has 1 limb.
        r.m_limbs[0]
            = ::mpn_divrem_1(q.m_limbs.data(), ::mp_size_t(0), data1, static_cast<::mp_size_t>(asize1), data2[0]);
    } else {
        // General implementation.
        ::mpn_tdiv_qr(q.m_limbs.data(), r.m_limbs.data(), ::mp_size_t(0), data1, static_cast<::mp_size_t>(asize1),
                      data2, static_cast<::mp_size_t>(asize2));
    }
    // Complete the quotient: compute size and sign.
    q._mp_size = asize1 - asize2 + 1;
    while (q._mp_size && !(q.m_limbs[static_cast<std::size_t>(q._mp_size - 1)] & GMP_NUMB_MASK)) {
        --q._mp_size;
    }
    if (sign1 != sign2) {
        q._mp_size = -q._mp_size;
    }
    // Complete the remainder.
    r._mp_size = asize2;
    while (r._mp_size && !(r.m_limbs[static_cast<std::size_t>(r._mp_size - 1)] & GMP_NUMB_MASK)) {
        --r._mp_size;
    }
    if (sign1 == -1) {
        r._mp_size = -r._mp_size;
    }
}

// 1-limb optimisation.
template <std::size_t SSize>
inline void static_div_impl(static_int<SSize> &q, static_int<SSize> &r, const static_int<SSize> &op1,
                            const static_int<SSize> &op2, mpz_size_t, mpz_size_t, int sign1, int sign2,
                            const std::integral_constant<int, 1> &)
{
    // NOTE: here we have to use GMP_NUMB_MASK because if s_size is 1 this implementation is *always*
    // called, even if we have nail bits (whereas the optimisation for other operations currently kicks in
    // only without nail bits). Thus, we need to discard from m_limbs[0] the nail bits before doing the
    // division.
    const ::mp_limb_t q_ = (op1.m_limbs[0] & GMP_NUMB_MASK) / (op2.m_limbs[0] & GMP_NUMB_MASK),
                      r_ = (op1.m_limbs[0] & GMP_NUMB_MASK) % (op2.m_limbs[0] & GMP_NUMB_MASK);
    // Write q.
    q._mp_size = sign1 * sign2 * (q_ != 0u);
    // NOTE: there should be no need here to mask.
    q.m_limbs[0] = q_;
    // Write r.
    // Following C++11, the sign of r is the sign of op1:
    // http://stackoverflow.com/questions/13100711/operator-modulo-change-in-c-11
    r._mp_size = sign1 * (r_ != 0u);
    r.m_limbs[0] = r_;
}

#if defined(MPPP_UINT128) && (GMP_NUMB_BITS == 64) && !GMP_NAIL_BITS
inline void dlimb_div(::mp_limb_t op11, ::mp_limb_t op12, ::mp_limb_t op21, ::mp_limb_t op22,
                      ::mp_limb_t *MPPP_RESTRICT q1, ::mp_limb_t *MPPP_RESTRICT q2, ::mp_limb_t *MPPP_RESTRICT r1,
                      ::mp_limb_t *MPPP_RESTRICT r2)
{
    using dlimb_t = MPPP_UINT128;
    const auto op1 = op11 + (dlimb_t(op12) << 64);
    const auto op2 = op21 + (dlimb_t(op22) << 64);
    const auto q = op1 / op2, r = op1 % op2;
    *q1 = static_cast<::mp_limb_t>(q & ::mp_limb_t(-1));
    *q2 = static_cast<::mp_limb_t>(q >> 64);
    *r1 = static_cast<::mp_limb_t>(r & ::mp_limb_t(-1));
    *r2 = static_cast<::mp_limb_t>(r >> 64);
}

// Without remainder.
inline void dlimb_div(::mp_limb_t op11, ::mp_limb_t op12, ::mp_limb_t op21, ::mp_limb_t op22,
                      ::mp_limb_t *MPPP_RESTRICT q1, ::mp_limb_t *MPPP_RESTRICT q2)
{
    using dlimb_t = MPPP_UINT128;
    const auto op1 = op11 + (dlimb_t(op12) << 64);
    const auto op2 = op21 + (dlimb_t(op22) << 64);
    const auto q = op1 / op2;
    *q1 = static_cast<::mp_limb_t>(q & ::mp_limb_t(-1));
    *q2 = static_cast<::mp_limb_t>(q >> 64);
}

#elif GMP_NUMB_BITS == 32 && !GMP_NAIL_BITS

inline void dlimb_div(::mp_limb_t op11, ::mp_limb_t op12, ::mp_limb_t op21, ::mp_limb_t op22,
                      ::mp_limb_t *MPPP_RESTRICT q1, ::mp_limb_t *MPPP_RESTRICT q2, ::mp_limb_t *MPPP_RESTRICT r1,
                      ::mp_limb_t *MPPP_RESTRICT r2)
{
    using dlimb_t = std::uint_least64_t;
    const auto op1 = op11 + (dlimb_t(op12) << 32);
    const auto op2 = op21 + (dlimb_t(op22) << 32);
    const auto q = op1 / op2, r = op1 % op2;
    *q1 = static_cast<::mp_limb_t>(q & ::mp_limb_t(-1));
    *q2 = static_cast<::mp_limb_t>(q >> 32);
    *r1 = static_cast<::mp_limb_t>(r & ::mp_limb_t(-1));
    *r2 = static_cast<::mp_limb_t>(r >> 32);
}

inline void dlimb_div(::mp_limb_t op11, ::mp_limb_t op12, ::mp_limb_t op21, ::mp_limb_t op22,
                      ::mp_limb_t *MPPP_RESTRICT q1, ::mp_limb_t *MPPP_RESTRICT q2)
{
    using dlimb_t = std::uint_least64_t;
    const auto op1 = op11 + (dlimb_t(op12) << 32);
    const auto op2 = op21 + (dlimb_t(op22) << 32);
    const auto q = op1 / op2;
    *q1 = static_cast<::mp_limb_t>(q & ::mp_limb_t(-1));
    *q2 = static_cast<::mp_limb_t>(q >> 32);
}

#endif

// 2-limbs optimisation.
template <std::size_t SSize>
inline void static_div_impl(static_int<SSize> &q, static_int<SSize> &r, const static_int<SSize> &op1,
                            const static_int<SSize> &op2, mpz_size_t asize1, mpz_size_t asize2, int sign1, int sign2,
                            const std::integral_constant<int, 2> &)
{
    if (asize1 < 2 && asize2 < 2) {
        // NOTE: testing indicates that it pays off to optimize the case in which the operands have
        // fewer than 2 limbs. This a slightly modified version of the 1-limb division from above,
        // without the need to mask as this function is called only if there are no nail bits.
        const ::mp_limb_t q_ = op1.m_limbs[0] / op2.m_limbs[0], r_ = op1.m_limbs[0] % op2.m_limbs[0];
        q._mp_size = sign1 * sign2 * (q_ != 0u);
        q.m_limbs[0] = q_;
        q.m_limbs[1] = 0u;
        r._mp_size = sign1 * (r_ != 0u);
        r.m_limbs[0] = r_;
        r.m_limbs[1] = 0u;
        return;
    }
    // Perform the division.
    ::mp_limb_t q1, q2, r1, r2;
    dlimb_div(op1.m_limbs[0], op1.m_limbs[1], op2.m_limbs[0], op2.m_limbs[1], &q1, &q2, &r1, &r2);
    // Write out.
    q._mp_size = sign1 * sign2 * (q2 ? 2 : (q1 ? 1 : 0));
    q.m_limbs[0] = q1;
    q.m_limbs[1] = q2;
    r._mp_size = sign1 * (r2 ? 2 : (r1 ? 1 : 0));
    r.m_limbs[0] = r1;
    r.m_limbs[1] = r2;
}

template <std::size_t SSize>
inline void static_div(static_int<SSize> &q, static_int<SSize> &r, const static_int<SSize> &op1,
                       const static_int<SSize> &op2)
{
    mpz_size_t asize1 = op1._mp_size, asize2 = op2._mp_size;
    int sign1 = asize1 != 0, sign2 = asize2 != 0;
    if (asize1 < 0) {
        asize1 = -asize1;
        sign1 = -1;
    }
    if (asize2 < 0) {
        asize2 = -asize2;
        sign2 = -1;
    }
    static_div_impl(q, r, op1, op2, asize1, asize2, sign1, sign2, integer_static_div_algo<static_int<SSize>>{});
    if (integer_static_div_algo<static_int<SSize>>::value == 0) {
        q.zero_unused_limbs();
        r.zero_unused_limbs();
    }
}

// Exact division.
// mpn implementation.
template <std::size_t SSize>
inline void static_divexact_impl(static_int<SSize> &q, const static_int<SSize> &op1, const static_int<SSize> &op2,
                                 mpz_size_t asize1, mpz_size_t asize2, int sign1, int sign2,
                                 const std::integral_constant<int, 0> &)
{
    if (asize1 == 0) {
        // Special casing if the numerator is zero (the mpn functions do not work with zero operands).
        q._mp_size = 0;
        return;
    }
#if __GNU_MP_VERSION > 6 || (__GNU_MP_VERSION == 6 && __GNU_MP_VERSION_MINOR >= 1)
    // NOTE: mpn_divexact_1() is available since GMP 6.1.0.
    if (asize2 == 1) {
        // Optimisation in case the dividend has only 1 limb.
        // NOTE: overlapping arguments are fine here.
        ::mpn_divexact_1(q.m_limbs.data(), op1.m_limbs.data(), static_cast<::mp_size_t>(asize1), op2.m_limbs[0]);
        // Complete the quotient: compute size and sign.
        q._mp_size = asize1 - asize2 + 1;
        while (q._mp_size && !(q.m_limbs[static_cast<std::size_t>(q._mp_size - 1)] & GMP_NUMB_MASK)) {
            --q._mp_size;
        }
        if (sign1 != sign2) {
            q._mp_size = -q._mp_size;
        }
        return;
    }
#else
    // Avoid compiler warnings for unused parameters.
    (void)sign1;
    (void)sign2;
    (void)asize2;
#endif
    // General implementation (via the mpz function).
    MPPP_MAYBE_TLS mpz_raii tmp;
    const auto v1 = op1.get_mpz_view();
    const auto v2 = op2.get_mpz_view();
    ::mpz_divexact(&tmp.m_mpz, &v1, &v2);
    // Copy over from the tmp struct into q.
    q._mp_size = tmp.m_mpz._mp_size;
    copy_limbs_no(tmp.m_mpz._mp_d, tmp.m_mpz._mp_d + (q._mp_size >= 0 ? q._mp_size : -q._mp_size), q.m_limbs.data());
}

// 1-limb optimisation.
template <std::size_t SSize>
inline void static_divexact_impl(static_int<SSize> &q, const static_int<SSize> &op1, const static_int<SSize> &op2,
                                 mpz_size_t, mpz_size_t, int sign1, int sign2, const std::integral_constant<int, 1> &)
{
    const ::mp_limb_t q_ = (op1.m_limbs[0] & GMP_NUMB_MASK) / (op2.m_limbs[0] & GMP_NUMB_MASK);
    // Write q.
    q._mp_size = sign1 * sign2 * (q_ != 0u);
    q.m_limbs[0] = q_;
}

// 2-limbs optimisation.
template <std::size_t SSize>
inline void static_divexact_impl(static_int<SSize> &q, const static_int<SSize> &op1, const static_int<SSize> &op2,
                                 mpz_size_t asize1, mpz_size_t asize2, int sign1, int sign2,
                                 const std::integral_constant<int, 2> &)
{
    if (asize1 < 2 && asize2 < 2) {
        // 1-limb optimisation.
        const ::mp_limb_t q_ = op1.m_limbs[0] / op2.m_limbs[0];
        q._mp_size = sign1 * sign2 * (q_ != 0u);
        q.m_limbs[0] = q_;
        q.m_limbs[1] = 0u;
        return;
    }
    // General case.
    ::mp_limb_t q1, q2;
    dlimb_div(op1.m_limbs[0], op1.m_limbs[1], op2.m_limbs[0], op2.m_limbs[1], &q1, &q2);
    q._mp_size = sign1 * sign2 * (q2 ? 2 : (q1 ? 1 : 0));
    q.m_limbs[0] = q1;
    q.m_limbs[1] = q2;
}

template <std::size_t SSize>
inline void static_divexact(static_int<SSize> &q, const static_int<SSize> &op1, const static_int<SSize> &op2)
{
    mpz_size_t asize1 = op1._mp_size, asize2 = op2._mp_size;
    int sign1 = asize1 != 0, sign2 = asize2 != 0;
    if (asize1 < 0) {
        asize1 = -asize1;
        sign1 = -1;
    }
    if (asize2 < 0) {
        asize2 = -asize2;
        sign2 = -1;
    }
    assert(asize1 == 0 || asize2 <= asize1);
    // NOTE: use integer_static_div_algo for the algorithm selection.
    static_divexact_impl(q, op1, op2, asize1, asize2, sign1, sign2, integer_static_div_algo<static_int<SSize>>{});
    if (integer_static_div_algo<static_int<SSize>>::value == 0) {
        q.zero_unused_limbs();
    }
}
}

/// Ternary truncated division.
/**
 * This function will set \p q to the truncated quotient <tt>n / d</tt> and \p r to
 * <tt>n % d</tt>. The remainder \p r has the same sign as \p n. \p q and \p r must be two distinct objects.
 *
 * @param q the quotient.
 * @param r the remainder.
 * @param n the dividend.
 * @param d the divisor.
 *
 * @throws std::invalid_argument if \p q and \p r are the same object.
 * @throws zero_division_error if \p d is zero.
 */
template <std::size_t SSize>
inline void tdiv_qr(integer<SSize> &q, integer<SSize> &r, const integer<SSize> &n, const integer<SSize> &d)
{
    if (mppp_unlikely(&q == &r)) {
        throw std::invalid_argument("When performing a division with remainder, the quotient 'q' and the "
                                    "remainder 'r' must be distinct objects");
    }
    if (mppp_unlikely(d.sgn() == 0)) {
        throw zero_division_error("Integer division by zero");
    }
    const bool sq = q.is_static(), sr = r.is_static(), s1 = n.is_static(), s2 = d.is_static();
    if (mppp_likely(s1 && s2)) {
        if (!sq) {
            q.set_zero();
        }
        if (!sr) {
            r.set_zero();
        }
        static_div(q._get_union().g_st(), r._get_union().g_st(), n._get_union().g_st(), d._get_union().g_st());
        // Division can never fail.
        return;
    }
    if (sq) {
        q._get_union().promote();
    }
    if (sr) {
        r._get_union().promote();
    }
    ::mpz_tdiv_qr(&q._get_union().g_dy(), &r._get_union().g_dy(), n.get_mpz_view(), d.get_mpz_view());
}

/// Exact division (ternary version).
/**
 * This function will set \p rop to the quotient of \p n and \p d.
 *
 * \rststar
 * .. warning::
 *
 *    If ``d`` does not divide ``n`` exactly, the behaviour will be undefined.
 * \endrststar
 *
 * @param rop the return value.
 * @param n the dividend.
 * @param d the divisor.
 *
 * @return a reference to \p rop.
 */
template <std::size_t SSize>
inline integer<SSize> &divexact(integer<SSize> &rop, const integer<SSize> &n, const integer<SSize> &d)
{
    const bool sr = rop.is_static(), s1 = n.is_static(), s2 = d.is_static();
    if (mppp_likely(s1 && s2)) {
        if (!sr) {
            rop.set_zero();
        }
        static_divexact(rop._get_union().g_st(), n._get_union().g_st(), d._get_union().g_st());
        // Division can never fail.
        return rop;
    }
    if (sr) {
        rop._get_union().promote();
    }
    ::mpz_divexact(&rop._get_union().g_dy(), n.get_mpz_view(), d.get_mpz_view());
    return rop;
}

/// Exact division (binary version).
/**
 * \rststar
 * .. warning::
 *
 *    If ``d`` does not divide ``n`` exactly, the behaviour will be undefined.
 * \endrststar
 *
 * @param n the dividend.
 * @param d the divisor.
 *
 * @return the quotient of \p n and \p d.
 */
template <std::size_t SSize>
inline integer<SSize> divexact(const integer<SSize> &n, const integer<SSize> &d)
{
    integer<SSize> retval;
    divexact(retval, n, d);
    return retval;
}

inline namespace detail
{

// Selection of the algorithm for static tdiv_q_2exp.
template <typename SInt>
using integer_static_tdiv_q_2exp_algo = std::integral_constant<int, (SInt::s_size > 2) ? 0 : SInt::s_size>;

// mpn implementation.
template <std::size_t SSize>
inline void static_tdiv_q_2exp_impl(static_int<SSize> &rop, const static_int<SSize> &n, ::mp_bitcnt_t s,
                                    const std::integral_constant<int, 0> &)
{
    mpz_size_t asize = n._mp_size;
    if (s == 0u || asize == 0) {
        // If shift is zero, or the operand is zero, write n into rop and return.
        rop = n;
        return;
    }
    // Finish setting up asize and sign.
    int sign = 1;
    if (asize < 0) {
        asize = -asize;
        sign = -1;
    }
    // ls: number of entire limbs shifted.
    // rs: effective shift that will be passed to the mpn function.
    const auto ls = s / unsigned(GMP_NUMB_BITS), rs = s % unsigned(GMP_NUMB_BITS);
    if (ls >= static_cast<std::size_t>(asize)) {
        // If we shift by a number of entire limbs equal to or larger than the asize,
        // the result will be zero.
        rop._mp_size = 0;
        return;
    }
    // The maximum new asize will be the old asize minus ls.
    const mpz_size_t new_asize = asize - static_cast<mpz_size_t>(ls);
    if (rs) {
        // Perform the shift via the mpn function, if we are effectively shifting at least 1 bit.
        // Overlapping is fine, as it is guaranteed that rop.m_limbs.data() <= n.m_limbs.data() + ls.
        ::mpn_rshift(rop.m_limbs.data(), n.m_limbs.data() + ls, static_cast<::mp_size_t>(new_asize), unsigned(rs));
    } else {
        // Otherwise, just copy over (the mpn function requires the shift to be at least 1).
        // NOTE: std::move requires that the destination iterator is not within the input range.
        // Here we are sure that ls > 0 because we don't have a remainder, and the zero shift case
        // was already handled above.
        assert(ls);
        std::move(n.m_limbs.begin() + ls, n.m_limbs.begin() + asize, rop.m_limbs.begin());
    }
    // Set the size. We need to check if the top limb is zero.
    rop._mp_size = sign * (new_asize - ((rop.m_limbs[static_cast<std::size_t>(new_asize - 1)] & GMP_NUMB_MASK) == 0u));
}

// 1-limb optimisation.
template <std::size_t SSize>
inline void static_tdiv_q_2exp_impl(static_int<SSize> &rop, const static_int<SSize> &n, ::mp_bitcnt_t s,
                                    const std::integral_constant<int, 1> &)
{
    const ::mp_limb_t l = n.m_limbs[0] & GMP_NUMB_MASK;
    if (s == 0u || l == 0u) {
        rop = n;
        return;
    }
    if (s >= unsigned(GMP_NUMB_BITS)) {
        // We are shifting by the limb's bit size or greater, the result will be zero.
        rop._mp_size = 0;
        rop.m_limbs[0] = 0u;
        return;
    }
    // Compute the result.
    const auto res = l >> s;
    // Size is the original one or zero.
    rop._mp_size = static_cast<int>(res != 0u) * n._mp_size;
    rop.m_limbs[0] = res;
}

// 2-limb optimisation.
template <std::size_t SSize>
inline void static_tdiv_q_2exp_impl(static_int<SSize> &rop, const static_int<SSize> &n, ::mp_bitcnt_t s,
                                    const std::integral_constant<int, 2> &)
{
    mpz_size_t asize = n._mp_size;
    if (s == 0u || asize == 0) {
        rop = n;
        return;
    }
    int sign = 1;
    if (asize < 0) {
        asize = -asize;
        sign = -1;
    }
    // If shift is too large, zero the result and return.
    static_assert(unsigned(GMP_NUMB_BITS) <= std::numeric_limits<unsigned>::max() / 2u, "Overflow error.");
    if (s >= 2u * unsigned(GMP_NUMB_BITS)) {
        rop._mp_size = 0;
        rop.m_limbs[0u] = 0u;
        rop.m_limbs[1u] = 0u;
        return;
    }
    if (s >= unsigned(GMP_NUMB_BITS)) {
        // NOTE: here the effective shift < GMP_NUMB_BITS, otherwise it would have been caught
        // in the check above.
        const auto lo = (n.m_limbs[1u] & GMP_NUMB_MASK) >> (s - unsigned(GMP_NUMB_BITS));
        // The size could be zero or +-1, depending
        // on the new content of m_limbs[0] and the previous
        // sign of _mp_size.
        rop._mp_size = static_cast<int>(lo != 0u) * sign;
        rop.m_limbs[0u] = lo;
        rop.m_limbs[1u] = 0u;
        return;
    }
    assert(s > 0u && s < unsigned(GMP_NUMB_BITS));
    // This represents the bits in hi that will be shifted down into lo.
    // We move them up so we can add tmp to the new lo to account for them.
    // NOTE: mask the final result to avoid spillover into potential nail bits.
    const auto tmp = ((n.m_limbs[1u] & GMP_NUMB_MASK) << (unsigned(GMP_NUMB_BITS) - s)) & GMP_NUMB_MASK;
    rop.m_limbs[0u] = ((n.m_limbs[0u] & GMP_NUMB_MASK) >> s) + tmp;
    rop.m_limbs[1u] = (n.m_limbs[1u] & GMP_NUMB_MASK) >> s;
    // The effective shift was less than 1 entire limb. The new asize must be the old one,
    // or one less than that.
    rop._mp_size = sign * (asize - ((rop.m_limbs[std::size_t(asize - 1)] & GMP_NUMB_MASK) == 0u));
}

template <std::size_t SSize>
inline void static_tdiv_q_2exp(static_int<SSize> &rop, const static_int<SSize> &n, ::mp_bitcnt_t s)
{
    static_tdiv_q_2exp_impl(rop, n, s, integer_static_tdiv_q_2exp_algo<static_int<SSize>>{});
    if (integer_static_tdiv_q_2exp_algo<static_int<SSize>>::value == 0) {
        rop.zero_unused_limbs();
    }
}
}

/// Ternary right shift.
/**
 * This function will set \p rop to \p n divided by <tt>2**s</tt>. \p rop will be the truncated result of the
 * division.
 *
 * @param rop the return value.
 * @param n the dividend.
 * @param s the bit shift value.
 *
 * @return a reference to \p rop.
 */
template <std::size_t SSize>
inline integer<SSize> &tdiv_q_2exp(integer<SSize> &rop, const integer<SSize> &n, ::mp_bitcnt_t s)
{
    const bool sn = n.is_static();
    bool sr = rop.is_static();
    if (mppp_likely(sn)) {
        if (!sr) {
            rop.set_zero();
            sr = true;
        }
        static_tdiv_q_2exp(rop._get_union().g_st(), n._get_union().g_st(), s);
        return rop;
    }
    if (sr) {
        rop._get_union().promote();
    }
    ::mpz_tdiv_q_2exp(&rop._get_union().g_dy(), n.get_mpz_view(), s);
    return rop;
}

/** @} */

/** @defgroup integer_comparison integer_comparison
 *  @{
 */

inline namespace detail
{

// Selection of the algorithm for static cmp.
template <typename SInt>
using integer_static_cmp_algo = std::integral_constant<int, SInt::s_size == 1 ? 1 : (SInt::s_size == 2 ? 2 : 0)>;

// mpn implementation.
template <std::size_t SSize>
inline int static_cmp(const static_int<SSize> &n1, const static_int<SSize> &n2, const std::integral_constant<int, 0> &)
{
    if (n1._mp_size < n2._mp_size) {
        return -1;
    }
    if (n2._mp_size < n1._mp_size) {
        return 1;
    }
    // The two sizes are equal, compare the absolute values.
    const auto asize = n1._mp_size >= 0 ? n1._mp_size : -n1._mp_size;
    if (asize == 0) {
        // Both operands are zero.
        // NOTE: we do this special casing in order to avoid calling mpn_cmp() on zero operands. It seems to
        // work, but the official GMP docs say one is not supposed to call mpn functions on zero operands.
        return 0;
    }
    const int cmp_abs = ::mpn_cmp(n1.m_limbs.data(), n2.m_limbs.data(), static_cast<::mp_size_t>(asize));
    // If the values are non-negative, return the comparison of the absolute values, otherwise invert it.
    return (n1._mp_size >= 0) ? cmp_abs : -cmp_abs;
}

// 1-limb optimisation.
template <std::size_t SSize>
inline int static_cmp(const static_int<SSize> &n1, const static_int<SSize> &n2, const std::integral_constant<int, 1> &)
{
    if (n1._mp_size < n2._mp_size) {
        return -1;
    }
    if (n2._mp_size < n1._mp_size) {
        return 1;
    }
    int cmp_abs = (n1.m_limbs[0u] & GMP_NUMB_MASK) > (n2.m_limbs[0u] & GMP_NUMB_MASK);
    if (!cmp_abs) {
        cmp_abs = -static_cast<int>((n1.m_limbs[0u] & GMP_NUMB_MASK) < (n2.m_limbs[0u] & GMP_NUMB_MASK));
    }
    return (n1._mp_size >= 0) ? cmp_abs : -cmp_abs;
}

// 2-limb optimisation.
template <std::size_t SSize>
inline int static_cmp(const static_int<SSize> &n1, const static_int<SSize> &n2, const std::integral_constant<int, 2> &)
{
    if (n1._mp_size < n2._mp_size) {
        return -1;
    }
    if (n2._mp_size < n1._mp_size) {
        return 1;
    }
    auto asize = n1._mp_size >= 0 ? n1._mp_size : -n1._mp_size;
    while (asize != 0) {
        --asize;
        if ((n1.m_limbs[std::size_t(asize)] & GMP_NUMB_MASK) > (n2.m_limbs[std::size_t(asize)] & GMP_NUMB_MASK)) {
            return (n1._mp_size >= 0) ? 1 : -1;
        }
        if ((n1.m_limbs[std::size_t(asize)] & GMP_NUMB_MASK) < (n2.m_limbs[std::size_t(asize)] & GMP_NUMB_MASK)) {
            return (n1._mp_size >= 0) ? -1 : 1;
        }
    }
    return 0;
}
}

/// Comparison function for integer.
/**
 * @param op1 first argument.
 * @param op2 second argument.
 *
 * @return \p 0 if <tt>op1 == op2</tt>, a negative value if <tt>op1 < op2</tt>, a positive value if
 * <tt>op1 > op2</tt>.
 */
template <std::size_t SSize>
inline int cmp(const integer<SSize> &op1, const integer<SSize> &op2)
{
    const bool s1 = op1.is_static(), s2 = op2.is_static();
    if (mppp_likely(s1 && s2)) {
        return static_cmp(op1._get_union().g_st(), op2._get_union().g_st(),
                          integer_static_cmp_algo<static_int<SSize>>{});
    }
    return ::mpz_cmp(op1.get_mpz_view(), op2.get_mpz_view());
}

/// Sign function.
/**
 * @param n the integer whose sign will be computed.
 *
 * @return 0 if \p n is zero, 1 if \p n is positive, -1 if \p n is negative.
 */
template <std::size_t SSize>
inline int sgn(const integer<SSize> &n)
{
    return n.sgn();
}

/// Test if integer is odd.
/**
 * @param n the argument.
 *
 * @return \p true if \p n is odd, \p false otherwise.
 */
template <std::size_t SSize>
inline bool odd_p(const integer<SSize> &n)
{
    return n.odd_p();
}

/// Test if integer is even.
/**
 * @param n the argument.
 *
 * @return \p true if \p n is even, \p false otherwise.
 */
template <std::size_t SSize>
inline bool even_p(const integer<SSize> &n)
{
    return n.even_p();
}

/// Test if an integer is zero.
/**
 * @param n the integer to be tested.
 *
 * @return \p true if \p n is zero, \p false otherwise.
 */
template <std::size_t SSize>
inline bool is_zero(const integer<SSize> &n)
{
    return n.is_zero();
}

/// Test if an integer is equal to one.
/**
 * @param n the integer to be tested.
 *
 * @return \p true if \p n is equal to 1, \p false otherwise.
 */
template <std::size_t SSize>
inline bool is_one(const integer<SSize> &n)
{
    return n.is_one();
}

/// Test if an integer is equal to minus one.
/**
 * @param n the integer to be tested.
 *
 * @return \p true if \p n is equal to -1, \p false otherwise.
 */
template <std::size_t SSize>
inline bool is_negative_one(const integer<SSize> &n)
{
    return n.is_negative_one();
}

/** @} */

/** @defgroup integer_ntheory integer_ntheory
 *  @{
 */

inline namespace detail
{

template <std::size_t SSize>
inline void static_gcd(static_int<SSize> &rop, const static_int<SSize> &op1, const static_int<SSize> &op2)
{
    mpz_size_t asize1 = op1._mp_size, asize2 = op2._mp_size;
    if (asize1 < 0) {
        asize1 = -asize1;
    }
    if (asize2 < 0) {
        asize2 = -asize2;
    }
    // Handle zeroes.
    if (!asize1) {
        rop._mp_size = asize2;
        rop.m_limbs = op2.m_limbs;
        return;
    }
    if (!asize2) {
        rop._mp_size = asize1;
        rop.m_limbs = op1.m_limbs;
        return;
    }
    // Special casing if an operand has asize 1.
    if (asize1 == 1) {
        rop._mp_size = 1;
        rop.m_limbs[0] = ::mpn_gcd_1(op2.m_limbs.data(), static_cast<::mp_size_t>(asize2), op1.m_limbs[0]);
        return;
    }
    if (asize2 == 1) {
        rop._mp_size = 1;
        rop.m_limbs[0] = ::mpn_gcd_1(op1.m_limbs.data(), static_cast<::mp_size_t>(asize1), op2.m_limbs[0]);
        return;
    }
    // General case, via mpz.
    // NOTE: there is an mpn_gcd() function, but it seems difficult to use. Apparently, and contrary to
    // what stated in the latest documentation, the mpn function requires odd operands and bit size
    // (not only limb size!) of the second operand not greater than the first. See for instance the old
    // documentation:
    // ftp://ftp.gnu.org/old-gnu/Manuals/gmp-3.1.1/html_chapter/gmp_9.html
    // Indeed, compiling GMP in debug mode and then trying to use the mpn function without respecting the above
    // results in assertion failures. For now let's keep it like this, the small operand cases are handled above
    // (partially) via mpn_gcd_1(), and in the future we can also think about binary GCD for 1/2 limbs optimisation.
    MPPP_MAYBE_TLS mpz_raii tmp;
    const auto v1 = op1.get_mpz_view();
    const auto v2 = op2.get_mpz_view();
    ::mpz_gcd(&tmp.m_mpz, &v1, &v2);
    // Copy over.
    rop._mp_size = tmp.m_mpz._mp_size;
    assert(rop._mp_size > 0);
    copy_limbs_no(tmp.m_mpz._mp_d, tmp.m_mpz._mp_d + rop._mp_size, rop.m_limbs.data());
}
}

/// GCD (ternary version).
/**
 * This function will set \p rop to the GCD of \p op1 and \p op2. The result is always positive.
 * If both operands are zero, zero is returned.
 *
 * @param rop the return value.
 * @param op1 the first operand.
 * @param op2 the second operand.
 *
 * @return a reference to \p rop.
 */
template <std::size_t SSize>
inline integer<SSize> &gcd(integer<SSize> &rop, const integer<SSize> &op1, const integer<SSize> &op2)
{
    const bool sr = rop.is_static(), s1 = op1.is_static(), s2 = op2.is_static();
    if (mppp_likely(s1 && s2)) {
        if (!sr) {
            rop.set_zero();
        }
        static_gcd(rop._get_union().g_st(), op1._get_union().g_st(), op2._get_union().g_st());
        rop._get_union().g_st().zero_unused_limbs();
        return rop;
    }
    if (sr) {
        rop._get_union().promote();
    }
    ::mpz_gcd(&rop._get_union().g_dy(), op1.get_mpz_view(), op2.get_mpz_view());
    return rop;
}

/// GCD (binary version).
/**
 * @param op1 the first operand.
 * @param op2 the second operand.
 *
 * @return the GCD of \p op1 and \p op2.
 */
template <std::size_t SSize>
inline integer<SSize> gcd(const integer<SSize> &op1, const integer<SSize> &op2)
{
    integer<SSize> retval;
    gcd(retval, op1, op2);
    return retval;
}

/// Factorial.
/**
 * This function will set \p rop to the factorial of \p n.
 *
 * @param rop the return value.
 * @param n the argument for the factorial.
 *
 * @return a reference to \p rop.
 *
 * @throws std::invalid_argument if \p n is larger than an implementation-defined limit.
 */
template <std::size_t SSize>
inline integer<SSize> &fac_ui(integer<SSize> &rop, unsigned long n)
{
    // NOTE: we put a limit here because the GMP function just crashes and burns
    // if n is too large, and n does not even need to be that large.
    constexpr auto max_fac = 1000000ull;
    if (mppp_unlikely(n > max_fac)) {
        throw std::invalid_argument(
            "The value " + std::to_string(n)
            + " is too large to be used as input for the factorial function (the maximum allowed value is "
            + std::to_string(max_fac) + ")");
    }
    // NOTE: let's get through a static temporary and then assign it to the rop,
    // so that rop will be static/dynamic according to the size of tmp.
    MPPP_MAYBE_TLS mpz_raii tmp;
    ::mpz_fac_ui(&tmp.m_mpz, n);
    return rop = &tmp.m_mpz;
}

/// Binomial coefficient (ternary version).
/**
 * This function will set \p rop to the binomial coefficient of \p n and \p k. Negative values of \p n are
 * supported.
 *
 * @param rop the return value.
 * @param n the top argument.
 * @param k the bottom argument.
 *
 * @return a reference to \p rop.
 */
template <std::size_t SSize>
inline integer<SSize> &bin_ui(integer<SSize> &rop, const integer<SSize> &n, unsigned long k)
{
    MPPP_MAYBE_TLS mpz_raii tmp;
    ::mpz_bin_ui(&tmp.m_mpz, n.get_mpz_view(), k);
    return rop = &tmp.m_mpz;
}

/// Binomial coefficient (binary version).
/**
 * @param n the top argument.
 * @param k the bottom argument.
 *
 * @return the binomial coefficient of \p n and \p k.
 */
template <std::size_t SSize>
inline integer<SSize> bin_ui(const integer<SSize> &n, unsigned long k)
{
    integer<SSize> retval;
    bin_ui(retval, n, k);
    return retval;
}

inline namespace detail
{

// These helpers are used here and in pow() as well.
template <typename T, enable_if_t<std::is_integral<T>::value, int> = 0>
inline unsigned long integer_exp_to_ulong(const T &exp)
{
#if !defined(__INTEL_COMPILER)
    assert(exp >= T(0));
#endif
    // NOTE: make_unsigned_t<T> is T if T is already unsigned.
    if (mppp_unlikely(static_cast<make_unsigned_t<T>>(exp) > std::numeric_limits<unsigned long>::max())) {
        throw std::overflow_error("Cannot convert the integral value " + std::to_string(exp)
                                  + " to unsigned long: the value is too large.");
    }
    return static_cast<unsigned long>(exp);
}

template <std::size_t SSize>
inline unsigned long integer_exp_to_ulong(const integer<SSize> &exp)
{
    try {
        return static_cast<unsigned long>(exp);
    } catch (const std::overflow_error &) {
        // Rewrite the error message.
        throw std::overflow_error("Cannot convert the integral value " + exp.to_string()
                                  + " to unsigned long: the value is too large.");
    }
}

template <typename T, std::size_t SSize>
inline integer<SSize> binomial_impl(const integer<SSize> &n, const T &k)
{
    // NOTE: here we re-use some helper methods used in the implementation of pow().
    if (sgn(k) >= 0) {
        return bin_ui(n, integer_exp_to_ulong(k));
    }
    // This is the case k < 0, handled according to:
    // http://arxiv.org/abs/1105.3689/
    if (n.sgn() >= 0) {
        // n >= 0, k < 0.
        return integer<SSize>{};
    }
    // n < 0, k < 0.
    if (k <= n) {
        // The formula is: (-1)**(n-k) * binomial(-k-1,n-k).
        // Cache n-k.
        const integer<SSize> nmk{n - k};
        integer<SSize> tmp{k};
        ++tmp;
        tmp.neg();
        auto retval = bin_ui(tmp, integer_exp_to_ulong(nmk));
        if (nmk.odd_p()) {
            retval.neg();
        }
        return retval;
    }
    return integer<SSize>{};
}

template <typename T, std::size_t SSize, enable_if_t<std::is_integral<T>::value, int> = 0>
inline integer<SSize> binomial_impl(const T &n, const integer<SSize> &k)
{
    return binomial_impl(integer<SSize>{n}, k);
}
}

/// Generic binomial coefficient.
/**
 * \rststar
 * This function is enabled only if ``T`` and ``U`` satisfy :cpp:concept:`~mppp::IntegerIntegralOpTypes`.
 * The return type is :cpp:class:`~mppp::integer`.
 *
 * This function will compute the binomial coefficient :math:`{{n}\choose{k}}`, supporting integral input values.
 * The implementation can handle positive and negative values for both the top and the bottom argument.
 *
 * .. seealso::
 *
 *    http://arxiv.org/abs/1105.3689/
 *
 * \endrststar
 *
 * @param n the top argument.
 * @param k the bottom argument.
 *
 * @return \f$ {{n}\choose{k}} \f$.
 *
 * @throws std::overflow_error if \p k is outside an implementation-defined range.
 */
#if defined(MPPP_HAVE_CONCEPTS)
template <typename T, typename U>
#if !defined(MPPP_DOXYGEN_INVOKED)
requires IntegerIntegralOpTypes<T, U>
#endif
#else
template <typename T, typename U, integer_integral_op_types_enabler<T, U> = 0>
#endif
    inline integer_common_t<T, U> binomial(const T &n, const U &k)
{
    return binomial_impl(n, k);
}

inline namespace detail
{

template <std::size_t SSize>
inline void nextprime_impl(integer<SSize> &rop, const integer<SSize> &n)
{
    MPPP_MAYBE_TLS mpz_raii tmp;
    ::mpz_nextprime(&tmp.m_mpz, n.get_mpz_view());
    rop = &tmp.m_mpz;
}
}

/// Compute next prime number (binary version).
/**
 * This function will set \p rop to the first prime number greater than \p n.
 * Note that for negative values of \p n this function always returns 2.
 *
 * @param rop the return value.
 * @param n the integer argument.
 *
 * @return a reference to \p rop.
 */
template <std::size_t SSize>
inline integer<SSize> &nextprime(integer<SSize> &rop, const integer<SSize> &n)
{
    // NOTE: nextprime on negative numbers always returns 2.
    nextprime_impl(rop, n);
    return rop;
}

/// Compute next prime number (unary version).
/**
 * @param n the integer argument.
 *
 * @return the first prime number greater than \p n.
 */
template <std::size_t SSize>
inline integer<SSize> nextprime(const integer<SSize> &n)
{
    integer<SSize> retval;
    nextprime_impl(retval, n);
    return retval;
}

/// Test primality.
/**
 * \rststar
 * This is the free-function version of :cpp:func:`mppp::integer::probab_prime_p()`.
 * \endrststar
 *
 * @param n the integer whose primality will be tested.
 * @param reps the number of tests to run.
 *
 * @return an integer indicating if \p n is a prime.
 *
 * @throws unspecified any exception thrown by integer::probab_prime_p().
 */
template <std::size_t SSize>
inline int probab_prime_p(const integer<SSize> &n, int reps = 25)
{
    return n.probab_prime_p(reps);
}

/** @} */

/** @defgroup integer_exponentiation integer_exponentiation
 *  @{
 */

/// Ternary integer exponentiation.
/**
 * This function will set \p rop to <tt>base**exp</tt>.
 *
 * @param rop the return value.
 * @param base the base.
 * @param exp the exponent.
 *
 * @return a reference to \p rop.
 */
template <std::size_t SSize>
inline integer<SSize> &pow_ui(integer<SSize> &rop, const integer<SSize> &base, unsigned long exp)
{
    MPPP_MAYBE_TLS mpz_raii tmp;
    ::mpz_pow_ui(&tmp.m_mpz, base.get_mpz_view(), exp);
    return rop = &tmp.m_mpz;
}

/// Binary integer exponentiation.
/**
 * @param base the base.
 * @param exp the exponent.
 *
 * @return <tt>base**exp</tt>.
 */
template <std::size_t SSize>
inline integer<SSize> pow_ui(const integer<SSize> &base, unsigned long exp)
{
    integer<SSize> retval;
    pow_ui(retval, base, exp);
    return retval;
}

inline namespace detail
{

// Various helpers for the implementation of pow().
template <typename T, enable_if_t<std::is_integral<T>::value, int> = 0>
inline bool integer_exp_is_odd(const T &exp)
{
    return (exp % T(2)) != T(0);
}

template <std::size_t SSize>
inline bool integer_exp_is_odd(const integer<SSize> &exp)
{
    return exp.odd_p();
}

// Implementation of pow().
// integer -- integral overload.
template <typename T, std::size_t SSize,
          enable_if_t<disjunction<std::is_same<T, integer<SSize>>, std::is_integral<T>>::value, int> = 0>
inline integer<SSize> pow_impl(const integer<SSize> &base, const T &exp)
{
    integer<SSize> rop;
    if (sgn(exp) >= 0) {
        pow_ui(rop, base, integer_exp_to_ulong(exp));
    } else if (mppp_unlikely(is_zero(base))) {
        // 0**-n is a division by zero.
        throw zero_division_error("Cannot raise zero to the negative power " + to_string(exp));
    } else if (base.is_one()) {
        // 1**n == 1.
        rop = 1;
    } else if (base.is_negative_one()) {
        if (integer_exp_is_odd(exp)) {
            // 1**(-2n-1) == -1.
            rop = -1;
        } else {
            // 1**(-2n) == 1.
            rop = 1;
        }
    } else {
        // m**-n == 1 / m**n == 0.
        rop = 0;
    }
    return rop;
}

// C++ integral -- integer overload.
template <typename T, std::size_t SSize, enable_if_t<std::is_integral<T>::value, int> = 0>
inline integer<SSize> pow_impl(const T &base, const integer<SSize> &exp)
{
    return pow_impl(integer<SSize>{base}, exp);
}

// integer -- FP overload.
template <typename T, std::size_t SSize, enable_if_t<std::is_floating_point<T>::value, int> = 0>
inline T pow_impl(const integer<SSize> &base, const T &exp)
{
    return std::pow(static_cast<T>(base), exp);
}

// FP -- integer overload.
template <typename T, std::size_t SSize, enable_if_t<std::is_floating_point<T>::value, int> = 0>
inline T pow_impl(const T &base, const integer<SSize> &exp)
{
    return std::pow(base, static_cast<T>(exp));
}
}

/// Binary exponentiation.
/**
 * \rststar
 * This function is enabled only if ``T`` and ``U`` satisfy :cpp:concept:`~mppp::IntegerOpTypes`.
 * The return type is determined as follows:
 *
 * * if the non-:cpp:class:`~mppp::integer` argument is a floating-point type ``F``, then the
 *   type of the result is ``F``; otherwise,
 * * the type of the result is :cpp:class:`~mppp::integer`.
 *
 * This function will raise ``base`` to the power ``exp``, and return the result. If one of the arguments
 * is a floating-point value, then the result will be computed via ``std::pow()`` and it will also be a
 * floating-point value. Otherwise, the result will be an :cpp:class:`~mppp::integer`.
 * In case of a negative integral exponent and integral base, the result will be zero unless
 * the absolute value of ``base`` is 1.
 * \endrststar
 *
 * @param base the base.
 * @param exp the exponent.
 *
 * @return <tt>base**exp</tt>.
 *
 * @throws std::overflow_error if \p base and \p exp are integrals and \p exp is non-negative and outside the range
 * of <tt>unsigned long</tt>.
 * @throws zero_division_error if \p base and \p exp are integrals and \p base is zero and \p exp is negative.
 */
template <typename T, typename U>
inline integer_common_t<T, U> pow(const T &base, const U &exp)
{
    return pow_impl(base, exp);
}

/** @} */

/** @defgroup integer_roots integer_roots
 *  @{
 */

inline namespace detail
{

template <std::size_t SSize>
inline void sqrt_impl(integer<SSize> &rop, const integer<SSize> &n)
{
    if (mppp_unlikely(n._get_union().m_st._mp_size < 0)) {
        throw std::domain_error("Cannot compute the square root of the negative number " + n.to_string());
    }
    const bool sr = rop.is_static(), sn = n.is_static();
    if (mppp_likely(sn)) {
        if (!sr) {
            rop.set_zero();
        }
        static_int<SSize> &rs = rop._get_union().g_st();
        const static_int<SSize> &ns = n._get_union().g_st();
        // NOTE: we know this is not negative, from the check above.
        const mpz_size_t size = ns._mp_size;
        if (!size) {
            // Special casing for zero.
            rs._mp_size = 0;
            rs.zero_unused_limbs();
            return;
        }
        // In case of overlap we need to go through a tmp variable.
        std::array<::mp_limb_t, SSize> tmp;
        const bool overlap = (&rs == &ns);
        auto out_ptr = overlap ? tmp.data() : rs.m_limbs.data();
        ::mpn_sqrtrem(out_ptr, nullptr, ns.m_limbs.data(), static_cast<::mp_size_t>(size));
        // Compute the size of the output (which is ceil(size / 2)).
        const mpz_size_t new_size = size / 2 + size % 2;
        assert(!new_size || (out_ptr[new_size - 1] & GMP_NUMB_MASK));
        // Write out the result.
        rs._mp_size = new_size;
        if (overlap) {
            copy_limbs_no(out_ptr, out_ptr + new_size, rs.m_limbs.data());
        }
        // Clear out unused limbs.
        rs.zero_unused_limbs();
        return;
    }
    if (sr) {
        rop.promote();
    }
    ::mpz_sqrt(&rop._get_union().g_dy(), n.get_mpz_view());
}
}

/// Integer square root (binary version).
/**
 * This method will set \p rop to the integer square root of \p n.
 *
 * @param rop the return value.
 * @param n the integer whose integer square root will be computed.
 *
 * @return a reference to \p rop.
 *
 * @throws std::domain_error if \p n is negative.
 */
template <std::size_t SSize>
inline integer<SSize> &sqrt(integer<SSize> &rop, const integer<SSize> &n)
{
    sqrt_impl(rop, n);
    return rop;
}

/// Integer square root (unary version).
/**
 * @param n the integer whose integer square root will be computed.
 *
 * @return the integer square root of \p n.
 *
 * @throws std::domain_error if \p n is negative.
 */
template <std::size_t SSize>
inline integer<SSize> sqrt(const integer<SSize> &n)
{
    integer<SSize> retval;
    sqrt_impl(retval, n);
    return retval;
}

/** @} */

/** @defgroup integer_io integer_io
 *  @{
 */

/// Output stream operator.
/**
 * \rststar
 * This operator will print to the stream ``os`` the :cpp:class:`~mppp::integer` ``n`` in base 10. Internally it uses
 * the :cpp:func:`mppp::integer::to_string()` method.
 * \endrststar
 *
 * @param os the target stream.
 * @param n the input integer.
 *
 * @return a reference to \p os.
 *
 * @throws unspecified any exception thrown by integer::to_string().
 */
template <std::size_t SSize>
inline std::ostream &operator<<(std::ostream &os, const integer<SSize> &n)
{
    return os << n.to_string();
}

/// Input stream operator.
/**
 * \rststar
 * This operator is equivalent to extracting a line from the stream and assigning it to ``n``.
 * \endrststar
 *
 * @param is the input stream.
 * @param n the integer to which the string extracted from the stream will be assigned.
 *
 * @return a reference to \p is.
 *
 * @throws unspecified any exception thrown by \link mppp::integer integer\endlink's assignment operator from string.
 */
template <std::size_t SSize>
inline std::istream &operator>>(std::istream &is, integer<SSize> &n)
{
    MPPP_MAYBE_TLS std::string tmp_str;
    std::getline(is, tmp_str);
    n = tmp_str;
    return is;
}

/** @} */

/** @defgroup integer_other integer_other
 *  @{
 */

/// Hash value.
/**
 * \rststar
 * This function will return a hash value for ``n``. The hash value depends only on the value of ``n``
 * (and *not* on its storage type).
 *
 * A specialisation of the standard ``std::hash`` functor is also provided, so that it is possible to use
 * :cpp:class:`~mppp::integer` in standard unordered associative containers out of the box.
 * \endrststar
 *
 * @param n the integer whose hash value will be computed.
 *
 * @return a hash value for \p n.
 */
template <std::size_t SSize>
inline std::size_t hash(const integer<SSize> &n)
{
    const mpz_size_t size = n._get_union().m_st._mp_size;
    const std::size_t asize = size >= 0 ? static_cast<std::size_t>(size) : static_cast<std::size_t>(nint_abs(size));
    const ::mp_limb_t *ptr
        = n._get_union().is_static() ? n._get_union().g_st().m_limbs.data() : n._get_union().g_dy()._mp_d;
    // Init the retval as the signed size.
    auto retval = static_cast<std::size_t>(size);
    // Combine the limbs.
    for (std::size_t i = 0; i < asize; ++i) {
        // The hash combiner. This is lifted directly from Boost. See also:
        // http://www.open-std.org/jtc1/sc22/wg21/docs/papers/2014/n3876.pdf
        retval ^= (ptr[i] & GMP_NUMB_MASK) + std::size_t(0x9e3779b9) + (retval << 6) + (retval >> 2);
    }
    return retval;
}

/** @} */

/** @defgroup integer_operators integer_operators
 *  @{
 */

inline namespace detail
{

// Dispatching for the binary addition operator.
template <std::size_t SSize>
inline integer<SSize> dispatch_binary_add(const integer<SSize> &op1, const integer<SSize> &op2)
{
    integer<SSize> retval;
    add(retval, op1, op2);
    return retval;
}

template <std::size_t SSize, typename T, enable_if_t<is_supported_integral<T>::value, int> = 0>
inline integer<SSize> dispatch_binary_add(const integer<SSize> &op1, T n)
{
    integer<SSize> retval{n};
    add(retval, retval, op1);
    return retval;
}

template <std::size_t SSize, typename T, enable_if_t<is_supported_integral<T>::value, int> = 0>
inline integer<SSize> dispatch_binary_add(T n, const integer<SSize> &op2)
{
    return dispatch_binary_add(op2, n);
}

template <std::size_t SSize, typename T, enable_if_t<is_supported_float<T>::value, int> = 0>
inline T dispatch_binary_add(const integer<SSize> &op1, T x)
{
    return static_cast<T>(op1) + x;
}

template <std::size_t SSize, typename T, enable_if_t<is_supported_float<T>::value, int> = 0>
inline T dispatch_binary_add(T x, const integer<SSize> &op2)
{
    return dispatch_binary_add(op2, x);
}

// Dispatching for in-place add.
template <std::size_t SSize>
inline void dispatch_in_place_add(integer<SSize> &retval, const integer<SSize> &n)
{
    add(retval, retval, n);
}

template <std::size_t SSize, typename T, enable_if_t<is_supported_integral<T>::value, int> = 0>
inline void dispatch_in_place_add(integer<SSize> &retval, const T &n)
{
    add(retval, retval, integer<SSize>{n});
}

template <std::size_t SSize, typename T, enable_if_t<is_supported_float<T>::value, int> = 0>
inline void dispatch_in_place_add(integer<SSize> &retval, const T &x)
{
    retval = static_cast<T>(retval) + x;
}

template <typename T, std::size_t SSize, enable_if_t<is_cpp_interoperable<T>::value, int> = 0>
inline void dispatch_in_place_add(T &rop, const integer<SSize> &op)
{
    rop = static_cast<T>(rop + op);
}
}

/// Identity operator.
/**
 * @param n the integer that will be copied.
 *
 * @return a copy of \p n.
 */
template <std::size_t SSize>
inline integer<SSize> operator+(const integer<SSize> &n)
{
    // NOTE: here potentially we could avoid a copy via either
    // a universal reference or maybe passing by copy n and then
    // moving in. Not sure how critical this is. Same in the negated
    // copy operator.
    return n;
}

/// Binary addition operator.
/**
 * \rststar
 * This operator is enabled only if ``T`` and ``U`` satisfy :cpp:concept:`~mppp::IntegerOpTypes`.
 * The return type is determined as follows:
 *
 * * if the non-:cpp:class:`~mppp::integer` argument is a floating-point type ``F``, then the
 *   type of the result is ``F``; otherwise,
 * * the type of the result is :cpp:class:`~mppp::integer`.
 *
 * \endrststar
 *
 * @param op1 the first summand.
 * @param op2 the second summand.
 *
 * @return <tt>op1 + op2</tt>.
 */
template <typename T, typename U>
inline integer_common_t<T, U> operator+(const T &op1, const U &op2)
{
    return dispatch_binary_add(op1, op2);
}

/// In-place addition operator.
/**
 * @param rop the augend.
 * @param op the addend.
 *
 * @return a reference to \p rop.
 *
 * @throws unspecified any exception thrown by the assignment of a floating-point value to \p rop or
 * by the conversion operator of \link mppp::integer integer\endlink.
 */
#if defined(MPPP_HAVE_CONCEPTS)
template <typename T>
inline T &operator+=(T &rop, const IntegerOpTypes<T> &op)
#else
template <typename T, typename U, integer_op_types_enabler<T, U> = 0>
inline T &operator+=(T &rop, const U &op)
#endif
{
    dispatch_in_place_add(rop, op);
    return rop;
}

/// Prefix increment.
/**
 * This operator will increment \p n by one.
 *
 * @param n the integer that will be increased.
 *
 * @return a reference to \p n after the increment.
 */
template <std::size_t SSize>
inline integer<SSize> &operator++(integer<SSize> &n)
{
    add_ui(n, n, 1u);
    return n;
}

/// Suffix increment.
/**
 * This operator will increment \p n by one and return a copy of \p n as it was before the increment.
 *
 * @param n the integer that will be increased.
 *
 * @return a copy of \p n before the increment.
 */
template <std::size_t SSize>
inline integer<SSize> operator++(integer<SSize> &n, int)
{
    auto retval(n);
    ++n;
    return retval;
}

inline namespace detail
{

// Dispatching for the binary subtraction operator.
template <std::size_t SSize>
inline integer<SSize> dispatch_binary_sub(const integer<SSize> &op1, const integer<SSize> &op2)
{
    integer<SSize> retval;
    sub(retval, op1, op2);
    return retval;
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_integral<T>::value, int> = 0>
inline integer<SSize> dispatch_binary_sub(const integer<SSize> &op1, T n)
{
    integer<SSize> retval{n};
    sub(retval, retval, op1);
    retval.neg();
    return retval;
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_integral<T>::value, int> = 0>
inline integer<SSize> dispatch_binary_sub(T n, const integer<SSize> &op2)
{
    auto retval = dispatch_binary_sub(op2, n);
    retval.neg();
    return retval;
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_float<T>::value, int> = 0>
inline T dispatch_binary_sub(const integer<SSize> &op1, T x)
{
    return static_cast<T>(op1) - x;
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_float<T>::value, int> = 0>
inline T dispatch_binary_sub(T x, const integer<SSize> &op2)
{
    return -dispatch_binary_sub(op2, x);
}

// Dispatching for in-place sub.
template <std::size_t SSize>
inline void dispatch_in_place_sub(integer<SSize> &retval, const integer<SSize> &n)
{
    sub(retval, retval, n);
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_integral<T>::value, int> = 0>
inline void dispatch_in_place_sub(integer<SSize> &retval, const T &n)
{
    sub(retval, retval, integer<SSize>{n});
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_float<T>::value, int> = 0>
inline void dispatch_in_place_sub(integer<SSize> &retval, const T &x)
{
    retval = static_cast<T>(retval) - x;
}

template <typename T, std::size_t SSize, enable_if_t<is_cpp_interoperable<T>::value, int> = 0>
inline void dispatch_in_place_sub(T &rop, const integer<SSize> &op)
{
    rop = static_cast<T>(rop - op);
}
}

/// Negated copy.
/**
 * @param n the integer that will be negated.
 *
 * @return a negated copy of \p n.
 */
template <std::size_t SSize>
integer<SSize> operator-(const integer<SSize> &n)
{
    auto retval(n);
    retval.neg();
    return retval;
}

/// Binary subtraction operator.
/**
 * \rststar
 * This operator is enabled only if ``T`` and ``U`` satisfy :cpp:concept:`~mppp::IntegerOpTypes`.
 * The return type is determined as follows:
 *
 * * if the non-:cpp:class:`~mppp::integer` argument is a floating-point type ``F``, then the
 *   type of the result is ``F``; otherwise,
 * * the type of the result is :cpp:class:`~mppp::integer`.
 *
 * \endrststar
 *
 * @param op1 the first operand.
 * @param op2 the second operand.
 *
 * @return <tt>op1 - op2</tt>.
 */
template <typename T, typename U>
inline integer_common_t<T, U> operator-(const T &op1, const U &op2)
{
    return dispatch_binary_sub(op1, op2);
}

/// In-place subtraction operator.
/**
 * @param rop the minuend.
 * @param op the subtrahend.
 *
 * @return a reference to \p rop.
 *
 * @throws unspecified any exception thrown by the assignment of a floating-point value to \p rop or
 * by the conversion operator of \link mppp::integer integer\endlink.
 */
#if defined(MPPP_HAVE_CONCEPTS)
template <typename T>
inline T &operator-=(T &rop, const IntegerOpTypes<T> &op)
#else
template <typename T, typename U, integer_op_types_enabler<T, U> = 0>
inline T &operator-=(T &rop, const U &op)
#endif
{
    dispatch_in_place_sub(rop, op);
    return rop;
}

/// Prefix decrement.
/**
 * This operator will decrement \p n by one.
 *
 * @param n the integer that will be decreased.
 *
 * @return a reference to \p n after the decrement.
 */
template <std::size_t SSize>
inline integer<SSize> &operator--(integer<SSize> &n)
{
    sub_ui(n, n, 1u);
    return n;
}

/// Suffix decrement.
/**
 * This operator will decrement \p n by one and return a copy of \p n as it was before the decrement.
 *
 * @param n the integer that will be decreased.
 *
 * @return a copy of \p n before the decrement.
 */
template <std::size_t SSize>
inline integer<SSize> operator--(integer<SSize> &n, int)
{
    auto retval(n);
    --n;
    return retval;
}

inline namespace detail
{

// Dispatching for the binary multiplication operator.
template <std::size_t SSize>
inline integer<SSize> dispatch_binary_mul(const integer<SSize> &op1, const integer<SSize> &op2)
{
    integer<SSize> retval;
    mul(retval, op1, op2);
    return retval;
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_integral<T>::value, int> = 0>
inline integer<SSize> dispatch_binary_mul(const integer<SSize> &op1, T n)
{
    // NOTE: with respect to addition, here we separate the retval
    // from the operands. Having a separate destination is generally better
    // for multiplication.
    integer<SSize> retval;
    mul(retval, op1, integer<SSize>{n});
    return retval;
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_integral<T>::value, int> = 0>
inline integer<SSize> dispatch_binary_mul(T n, const integer<SSize> &op2)
{
    return dispatch_binary_mul(op2, n);
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_float<T>::value, int> = 0>
inline T dispatch_binary_mul(const integer<SSize> &op1, T x)
{
    return static_cast<T>(op1) * x;
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_float<T>::value, int> = 0>
inline T dispatch_binary_mul(T x, const integer<SSize> &op2)
{
    return dispatch_binary_mul(op2, x);
}

// Dispatching for in-place multiplication.
template <std::size_t SSize>
inline void dispatch_in_place_mul(integer<SSize> &retval, const integer<SSize> &n)
{
    mul(retval, retval, n);
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_integral<T>::value, int> = 0>
inline void dispatch_in_place_mul(integer<SSize> &retval, const T &n)
{
    mul(retval, retval, integer<SSize>{n});
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_float<T>::value, int> = 0>
inline void dispatch_in_place_mul(integer<SSize> &retval, const T &x)
{
    retval = static_cast<T>(retval) * x;
}

template <typename T, std::size_t SSize, enable_if_t<is_cpp_interoperable<T>::value, int> = 0>
inline void dispatch_in_place_mul(T &rop, const integer<SSize> &op)
{
    rop = static_cast<T>(rop * op);
}
}

/// Binary multiplication operator.
/**
 * \rststar
 * This operator is enabled only if ``T`` and ``U`` satisfy :cpp:concept:`~mppp::IntegerOpTypes`.
 * The return type is determined as follows:
 *
 * * if the non-:cpp:class:`~mppp::integer` argument is a floating-point type ``F``, then the
 *   type of the result is ``F``; otherwise,
 * * the type of the result is :cpp:class:`~mppp::integer`.
 *
 * \endrststar
 *
 * @param op1 the first factor.
 * @param op2 the second factor.
 *
 * @return <tt>op1 * op2</tt>.
 */
template <typename T, typename U>
inline integer_common_t<T, U> operator*(const T &op1, const U &op2)
{
    return dispatch_binary_mul(op1, op2);
}

/// In-place multiplication operator.
/**
 * @param rop the multiplicand.
 * @param op the multiplicator.
 *
 * @return a reference to \p rop.
 *
 * @throws unspecified any exception thrown by the assignment of a floating-point value to \p rop or
 * by the conversion operator of \link mppp::integer integer\endlink.
 */
#if defined(MPPP_HAVE_CONCEPTS)
template <typename T>
inline T &operator*=(T &rop, const IntegerOpTypes<T> &op)
#else
template <typename T, typename U, integer_op_types_enabler<T, U> = 0>
inline T &operator*=(T &rop, const U &op)
#endif
{
    dispatch_in_place_mul(rop, op);
    return rop;
}

inline namespace detail
{

// Dispatching for the binary division operator.
template <std::size_t SSize>
inline integer<SSize> dispatch_binary_div(const integer<SSize> &op1, const integer<SSize> &op2)
{
    integer<SSize> retval, r;
    tdiv_qr(retval, r, op1, op2);
    return retval;
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_integral<T>::value, int> = 0>
inline integer<SSize> dispatch_binary_div(const integer<SSize> &op1, T n)
{
    integer<SSize> retval, r;
    tdiv_qr(retval, r, op1, integer<SSize>{n});
    return retval;
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_integral<T>::value, int> = 0>
inline integer<SSize> dispatch_binary_div(T n, const integer<SSize> &op2)
{
    integer<SSize> retval, r;
    tdiv_qr(retval, r, integer<SSize>{n}, op2);
    return retval;
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_float<T>::value, int> = 0>
inline T dispatch_binary_div(const integer<SSize> &op1, T x)
{
    return static_cast<T>(op1) / x;
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_float<T>::value, int> = 0>
inline T dispatch_binary_div(T x, const integer<SSize> &op2)
{
    return x / static_cast<T>(op2);
}

// Dispatching for in-place div.
template <std::size_t SSize>
inline void dispatch_in_place_div(integer<SSize> &retval, const integer<SSize> &n)
{
    integer<SSize> r;
    tdiv_qr(retval, r, retval, n);
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_integral<T>::value, int> = 0>
inline void dispatch_in_place_div(integer<SSize> &retval, const T &n)
{
    integer<SSize> r;
    tdiv_qr(retval, r, retval, integer<SSize>{n});
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_float<T>::value, int> = 0>
inline void dispatch_in_place_div(integer<SSize> &retval, const T &x)
{
    retval = static_cast<T>(retval) / x;
}

template <typename T, std::size_t SSize, enable_if_t<is_cpp_interoperable<T>::value, int> = 0>
inline void dispatch_in_place_div(T &rop, const integer<SSize> &op)
{
    rop = static_cast<T>(rop / op);
}

// Dispatching for the binary modulo operator.
template <std::size_t SSize>
inline integer<SSize> dispatch_binary_mod(const integer<SSize> &op1, const integer<SSize> &op2)
{
    integer<SSize> q, retval;
    tdiv_qr(q, retval, op1, op2);
    return retval;
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_integral<T>::value, int> = 0>
inline integer<SSize> dispatch_binary_mod(const integer<SSize> &op1, T n)
{
    integer<SSize> q, retval;
    tdiv_qr(q, retval, op1, integer<SSize>{n});
    return retval;
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_integral<T>::value, int> = 0>
inline integer<SSize> dispatch_binary_mod(T n, const integer<SSize> &op2)
{
    integer<SSize> q, retval;
    tdiv_qr(q, retval, integer<SSize>{n}, op2);
    return retval;
}

// Dispatching for in-place mod.
template <std::size_t SSize>
inline void dispatch_in_place_mod(integer<SSize> &retval, const integer<SSize> &n)
{
    integer<SSize> q;
    tdiv_qr(q, retval, retval, n);
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_integral<T>::value, int> = 0>
inline void dispatch_in_place_mod(integer<SSize> &retval, const T &n)
{
    integer<SSize> q;
    tdiv_qr(q, retval, retval, integer<SSize>{n});
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_integral<T>::value, int> = 0>
inline void dispatch_in_place_mod(T &rop, const integer<SSize> &op)
{
    rop = static_cast<T>(rop % op);
}
}

/// Binary division operator.
/**
 * \rststar
 * This operator is enabled only if ``T`` and ``U`` satisfy :cpp:concept:`~mppp::IntegerOpTypes`.
 * The return type is determined as follows:
 *
 * * if the non-:cpp:class:`~mppp::integer` argument is a floating-point type ``F``, then the
 *   type of the result is ``F``; otherwise,
 * * the type of the result is :cpp:class:`~mppp::integer`.
 *
 * \endrststar
 *
 * @param n the dividend.
 * @param d the divisor.
 *
 * @return <tt>n / d</tt>.
 *
 * @throws zero_division_error if \p d is zero and only integral types are involved in the division.
 */
template <typename T, typename U>
inline integer_common_t<T, U> operator/(const T &n, const U &d)
{
    return dispatch_binary_div(n, d);
}

/// In-place division operator.
/**
 * @param rop the dividend.
 * @param op the divisor.
 *
 * @return a reference to \p rop.
 *
 * @throws zero_division_error if \p op is zero and only integral types are involved in the division.
 * @throws unspecified any exception thrown by the assignment of a floating-point value to \p rop or
 * by the conversion operator of \link mppp::integer integer\endlink.
 */
#if defined(MPPP_HAVE_CONCEPTS)
template <typename T>
inline T &operator/=(T &rop, const IntegerOpTypes<T> &op)
#else
template <typename T, typename U, integer_op_types_enabler<T, U> = 0>
inline T &operator/=(T &rop, const U &op)
#endif
{
    dispatch_in_place_div(rop, op);
    return rop;
}

/// Binary modulo operator.
/**
 * \rststar
 * This operator is enabled only if ``T`` and ``U`` satisfy :cpp:concept:`~mppp::IntegerIntegralOpTypes`.
 * The return type is :cpp:class:`~mppp::integer`.
 * \endrststar
 *
 * @param n the dividend.
 * @param d the divisor.
 *
 * @return <tt>n % d</tt>.
 *
 * @throws zero_division_error if \p d is zero.
 */
#if defined(MPPP_HAVE_CONCEPTS)
template <typename T, typename U>
#if !defined(MPPP_DOXYGEN_INVOKED)
requires IntegerIntegralOpTypes<T, U>
#endif
#else
template <typename T, typename U, integer_integral_op_types_enabler<T, U> = 0>
#endif
    inline integer_common_t<T, U> operator%(const T &n, const U &d)
{
    return dispatch_binary_mod(n, d);
}

/// In-place modulo operator.
/**
 * @param rop the dividend.
 * @param op the divisor.
 *
 * @return a reference to \p rop.
 *
 * @throws zero_division_error if \p op is zero.
 * @throws unspecified any exception thrown by the conversion operator of \link mppp::integer integer\endlink.
 */
#if defined(MPPP_HAVE_CONCEPTS)
template <typename T>
inline T &operator%=(T &rop, const IntegerIntegralOpTypes<T> &op)
#else
template <typename T, typename U, integer_integral_op_types_enabler<T, U> = 0>
inline T &operator%=(T &rop, const U &op)
#endif
{
    dispatch_in_place_mod(rop, op);
    return rop;
}

#if !defined(MPPP_DOXYGEN_INVOKED)

template <typename T>
#if defined(MPPP_HAVE_CONCEPTS)
concept bool IntegerShiftType = CppInteroperable<T> &&std::is_integral<T>::value;
#else
using integer_shift_type_enabler = enable_if_t<conjunction<is_cpp_interoperable<T>, std::is_integral<T>>::value, int>;
#endif

#endif

/// Binary left shift operator.
/**
 * @param n the multiplicand.
 * @param s the bit shift value.
 *
 * @return \p n times <tt>2**s</tt>.
 *
 * @throws std::overflow_error if \p s is negative or larger than an implementation-defined value.
 */
#if defined(MPPP_HAVE_CONCEPTS)
template <std::size_t SSize>
inline integer<SSize> operator<<(const integer<SSize> &n, IntegerShiftType s)
#else
template <typename T, std::size_t SSize, integer_shift_type_enabler<T> = 0>
inline integer<SSize> operator<<(const integer<SSize> &n, T s)
#endif
{
    integer<SSize> retval;
    mul_2exp(retval, n, safe_cast<::mp_bitcnt_t>(s));
    return retval;
}

/// In-place left shift operator.
/**
 * @param rop the multiplicand.
 * @param s the bit shift value.
 *
 * @return a reference to \p rop.
 *
 * @throws std::overflow_error if \p s is negative or larger than an implementation-defined value.
 */
#if defined(MPPP_HAVE_CONCEPTS)
template <std::size_t SSize>
inline integer<SSize> &operator<<=(integer<SSize> &rop, IntegerShiftType s)
#else
template <typename T, std::size_t SSize, integer_shift_type_enabler<T> = 0>
inline integer<SSize> &operator<<=(integer<SSize> &rop, T s)
#endif
{
    mul_2exp(rop, rop, safe_cast<::mp_bitcnt_t>(s));
    return rop;
}

/// Binary right shift operator.
/**
 * @param n the dividend.
 * @param s the bit shift value.
 *
 * @return \p n divided <tt>2**s</tt>.
 *
 * @throws std::overflow_error if \p s is negative or larger than an implementation-defined value.
 */
#if defined(MPPP_HAVE_CONCEPTS)
template <std::size_t SSize>
inline integer<SSize> operator>>(const integer<SSize> &n, IntegerShiftType s)
#else
template <typename T, std::size_t SSize, integer_shift_type_enabler<T> = 0>
inline integer<SSize> operator>>(const integer<SSize> &n, T s)
#endif
{
    integer<SSize> retval;
    tdiv_q_2exp(retval, n, safe_cast<::mp_bitcnt_t>(s));
    return retval;
}

/// In-place right shift operator.
/**
 * @param rop the dividend.
 * @param s the bit shift value.
 *
 * @return a reference to \p rop.
 *
 * @throws std::overflow_error if \p s is negative or larger than an implementation-defined value.
 */
#if defined(MPPP_HAVE_CONCEPTS)
template <std::size_t SSize>
inline integer<SSize> &operator>>=(integer<SSize> &rop, IntegerShiftType s)
#else
template <typename T, std::size_t SSize, integer_shift_type_enabler<T> = 0>
inline integer<SSize> &operator>>=(integer<SSize> &rop, T s)
#endif
{
    tdiv_q_2exp(rop, rop, safe_cast<::mp_bitcnt_t>(s));
    return rop;
}

inline namespace detail
{

// Equality operator.
// NOTE: special implementation instead of using cmp, this should be faster.
template <std::size_t SSize>
inline bool dispatch_equality(const integer<SSize> &a, const integer<SSize> &b)
{
    const mp_size_t size_a = a._get_union().m_st._mp_size, size_b = b._get_union().m_st._mp_size;
    if (size_a != size_b) {
        return false;
    }
    const std::size_t asize
        = size_a >= 0 ? static_cast<std::size_t>(size_a) : static_cast<std::size_t>(nint_abs(size_a));
    const ::mp_limb_t *ptr_a = a.is_static() ? a._get_union().g_st().m_limbs.data() : a._get_union().g_dy()._mp_d;
    const ::mp_limb_t *ptr_b = b.is_static() ? b._get_union().g_st().m_limbs.data() : b._get_union().g_dy()._mp_d;
    auto limb_cmp
        = [](const ::mp_limb_t &l1, const ::mp_limb_t &l2) { return (l1 & GMP_NUMB_MASK) == (l2 & GMP_NUMB_MASK); };
#if defined(_MSC_VER)
    return std::equal(stdext::make_checked_array_iterator(ptr_a, asize),
                      stdext::make_checked_array_iterator(ptr_a, asize, asize),
                      stdext::make_checked_array_iterator(ptr_b, asize), limb_cmp);
#else
    return std::equal(ptr_a, ptr_a + asize, ptr_b, limb_cmp);
#endif
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_integral<T>::value, int> = 0>
inline bool dispatch_equality(const integer<SSize> &a, T n)
{
    return dispatch_equality(a, integer<SSize>{n});
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_integral<T>::value, int> = 0>
inline bool dispatch_equality(T n, const integer<SSize> &a)
{
    return dispatch_equality(a, n);
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_float<T>::value, int> = 0>
inline bool dispatch_equality(const integer<SSize> &a, T x)
{
    return static_cast<T>(a) == x;
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_float<T>::value, int> = 0>
inline bool dispatch_equality(T x, const integer<SSize> &a)
{
    return dispatch_equality(a, x);
}

// Less-than operator.
template <std::size_t SSize>
inline bool dispatch_less_than(const integer<SSize> &a, const integer<SSize> &b)
{
    return cmp(a, b) < 0;
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_integral<T>::value, int> = 0>
inline bool dispatch_less_than(const integer<SSize> &a, T n)
{
    return dispatch_less_than(a, integer<SSize>{n});
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_integral<T>::value, int> = 0>
inline bool dispatch_less_than(T n, const integer<SSize> &a)
{
    return dispatch_greater_than(a, integer<SSize>{n});
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_float<T>::value, int> = 0>
inline bool dispatch_less_than(const integer<SSize> &a, T x)
{
    return static_cast<T>(a) < x;
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_float<T>::value, int> = 0>
inline bool dispatch_less_than(T x, const integer<SSize> &a)
{
    return dispatch_greater_than(a, x);
}

// Greater-than operator.
template <std::size_t SSize>
inline bool dispatch_greater_than(const integer<SSize> &a, const integer<SSize> &b)
{
    return cmp(a, b) > 0;
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_integral<T>::value, int> = 0>
inline bool dispatch_greater_than(const integer<SSize> &a, T n)
{
    return dispatch_greater_than(a, integer<SSize>{n});
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_integral<T>::value, int> = 0>
inline bool dispatch_greater_than(T n, const integer<SSize> &a)
{
    return dispatch_less_than(a, integer<SSize>{n});
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_float<T>::value, int> = 0>
inline bool dispatch_greater_than(const integer<SSize> &a, T x)
{
    return static_cast<T>(a) > x;
}

template <typename T, std::size_t SSize, enable_if_t<is_supported_float<T>::value, int> = 0>
inline bool dispatch_greater_than(T x, const integer<SSize> &a)
{
    return dispatch_less_than(a, x);
}
}

/// Equality operator.
/**
 * @param op1 first argument.
 * @param op2 second argument.
 *
 * @return \p true if <tt>op1 == op2</tt>, \p false otherwise.
 */
#if defined(MPPP_HAVE_CONCEPTS)
template <typename T>
inline bool operator==(const T &op1, const IntegerOpTypes<T> &op2)
#else
template <typename T, typename U, integer_op_types_enabler<T, U> = 0>
inline bool operator==(const T &op1, const U &op2)
#endif
{
    return dispatch_equality(op1, op2);
}

/// Inequality operator.
/**
 * @param op1 first argument.
 * @param op2 second argument.
 *
 * @return \p true if <tt>op1 != op2</tt>, \p false otherwise.
 */
#if defined(MPPP_HAVE_CONCEPTS)
template <typename T>
inline bool operator!=(const T &op1, const IntegerOpTypes<T> &op2)
#else
template <typename T, typename U, integer_op_types_enabler<T, U> = 0>
inline bool operator!=(const T &op1, const U &op2)
#endif
{
    return !(op1 == op2);
}

/// Less-than operator.
/**
 * @param op1 first argument.
 * @param op2 second argument.
 *
 * @return \p true if <tt>op1 < op2</tt>, \p false otherwise.
 */
#if defined(MPPP_HAVE_CONCEPTS)
template <typename T>
inline bool operator<(const T &op1, const IntegerOpTypes<T> &op2)
#else
template <typename T, typename U, integer_op_types_enabler<T, U> = 0>
inline bool operator<(const T &op1, const U &op2)
#endif
{
    return dispatch_less_than(op1, op2);
}

/// Less-than or equal operator.
/**
 * @param op1 first argument.
 * @param op2 second argument.
 *
 * @return \p true if <tt>op1 <= op2</tt>, \p false otherwise.
 */
#if defined(MPPP_HAVE_CONCEPTS)
template <typename T>
inline bool operator<=(const T &op1, const IntegerOpTypes<T> &op2)
#else
template <typename T, typename U, integer_op_types_enabler<T, U> = 0>
inline bool operator<=(const T &op1, const U &op2)
#endif
{
    return !(op1 > op2);
}

/// Greater-than operator.
/**
 * @param op1 first argument.
 * @param op2 second argument.
 *
 * @return \p true if <tt>op1 > op2</tt>, \p false otherwise.
 */
#if defined(MPPP_HAVE_CONCEPTS)
template <typename T>
inline bool operator>(const T &op1, const IntegerOpTypes<T> &op2)
#else
template <typename T, typename U, integer_op_types_enabler<T, U> = 0>
inline bool operator>(const T &op1, const U &op2)
#endif
{
    return dispatch_greater_than(op1, op2);
}

/// Greater-than or equal operator.
/**
 * @param op1 first argument.
 * @param op2 second argument.
 *
 * @return \p true if <tt>op1 >= op2</tt>, \p false otherwise.
 */
#if defined(MPPP_HAVE_CONCEPTS)
template <typename T>
inline bool operator>=(const T &op1, const IntegerOpTypes<T> &op2)
#else
template <typename T, typename U, integer_op_types_enabler<T, U> = 0>
inline bool operator>=(const T &op1, const U &op2)
#endif
{
    return !(op1 < op2);
}

/** @} */
}

namespace std
{

/// Specialisation of \p std::hash for mppp::integer.
template <size_t SSize>
struct hash<mppp::integer<SSize>> {
// NOTE: these typedefs have been deprecated in C++17.
#if MPPP_CPLUSPLUS < 201703L
    /// The argument type.
    typedef mppp::integer<SSize> argument_type;
    /// The result type.
    typedef size_t result_type;
#endif
    /// Call operator.
    /**
     * @param n the integer whose hash will be returned.
     *
     * @return a hash value for \p n.
     */
    size_t operator()(const mppp::integer<SSize> &n) const
    {
        return mppp::hash(n);
    }
};
}

#if defined(_MSC_VER)

#pragma warning(pop)

#endif

#if defined(MPPP_UINT128)

#undef MPPP_UINT128

#endif

#endif
