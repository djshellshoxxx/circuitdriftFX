# Third-party notices

Only MIT/BSD/Apache/ISC/public-domain material may be included; every entry records source URL,
license and commit. Modules append their own sections below.

## SplitMix64 and xoshiro256** (src/control/CreativeMotion.cpp)
Sebastiano Vigna and David Blackman, https://prng.di.unimi.it/ (splitmix64.c, xoshiro256starstar.c). Public domain / CC0 1.0. Reimplemented from the reference algorithm; no code copied verbatim beyond constants.

## RBJ Audio EQ Cookbook (src/control/Macros.cpp)
Robert Bristow-Johnson biquad formulae, implemented from the published formulas; no code copied.

## cd_community (src/community)

No third-party code. `src/community/Sha256.cpp` is an original implementation written from the
FIPS 180-4 specification and verified against the NIST test vectors ("", "abc", the 448-bit and
896-bit messages, and one million repetitions of "a").
