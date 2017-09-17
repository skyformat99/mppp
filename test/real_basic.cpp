// Copyright 2016-2017 Francesco Biscani (bluescarni@gmail.com)
//
// This file is part of the mp++ library.
//
// This Source Code Form is subject to the terms of the Mozilla
// Public License v. 2.0. If a copy of the MPL was not distributed
// with this file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include <mp++/mp++.hpp>

#define CATCH_CONFIG_MAIN
#include "catch.hpp"

using namespace mppp;

TEST_CASE("real basic")
{
    std::cout << real{1ll} << '\n';
    std::cout << real{1.l} << '\n';
    real r{12356732};
    std::cout << r.prec_round(120) << '\n';
    std::cout << r.prec_round(12) << '\n';
    std::cout << real{true} << '\n';
    std::cout << real{integer<1>{1}} << '\n';
    std::cout << static_cast<integer<1>>(real{integer<1>{1}}) << '\n';
    std::cout << real{rational<1>{1, 3}} << '\n';
    std::cout << static_cast<rational<1>>(real{rational<1>{1, 3}}) << '\n';
    std::cout << static_cast<float>(real{rational<1>{1, 3}}) << '\n';
    std::cout << static_cast<double>(real{rational<1>{1, 3}}) << '\n';
    std::cout << static_cast<long double>(real{rational<1>{1, 3}}) << '\n';
    std::cout << static_cast<unsigned>(real{128}) << '\n';
    std::cout << static_cast<bool>(real{128}) << '\n';
    std::cout << static_cast<bool>(real{-128}) << '\n';
    std::cout << static_cast<bool>(real{0}) << '\n';
    std::cout << static_cast<int>(real{42}) << '\n';
    std::cout << static_cast<long long>(real{-42}) << '\n';
#if defined(MPPP_WITH_QUADMATH)
    std::cout << static_cast<real128>(real{-42}) << '\n';
    std::cout << static_cast<real128>(real{-1}) << '\n';
    std::cout << static_cast<real128>(real{1}) << '\n';
    std::cout << static_cast<real128>(real{real128{"3.40917866435610111081769936359662259e-4957"}}) << '\n';
    std::cout << static_cast<real128>(real{real128{"3.40917866435610111081769936359662259e-4957"}}.prec_round(127))
              << '\n';
    std::cout << static_cast<real128>(real{real128{"3.40917866435610111081769936359662259e-4957"}}.prec_round(128))
              << '\n';
    std::cout << static_cast<real128>(real{real128{"3.40917866435610111081769936359662259e-4957"}}.prec_round(129))
              << '\n';
    std::cout << static_cast<real128>(real{real128{"3.40917866435610111081769936359662259e-4957"}}.prec_round(250))
              << '\n';
    std::cout << real{-real128{"1.3E200"}} << '\n';
    std::cout << -real128{"1.3E200"} << '\n';
    std::cout << real{-real128{"1.3E-200"}} << '\n';
    std::cout << -real128{"1.3E-200"} << '\n';
    std::cout << real128{"1E-4940"} << '\n';
    std::cout << real{real128{"1E-4940"}} << '\n';
#endif
}