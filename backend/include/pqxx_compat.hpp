#pragma once
// Ubuntu 24.04's libpqxx 7.8 binary uses the C++17 exception ABI, while
// its headers enable source_location from the application's C++20 mode.
// CMake probes the actual linked library before selecting this compatibility path.
#ifdef LENS_PQXX_LEGACY_EXCEPTION_ABI
#include <version>
#pragma push_macro("__cpp_lib_source_location")
#undef __cpp_lib_source_location
#include <pqxx/pqxx>
#pragma pop_macro("__cpp_lib_source_location")
#else
#include <pqxx/pqxx>
#endif
