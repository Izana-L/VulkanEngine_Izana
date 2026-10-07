#ifndef SAFE_MATH_GLSL
#define SAFE_MATH_GLSL

// Operations whose GLSL definition leaves a value undefined (or NaN) for an
// input that real data can produce, written once so every shader handles
// the degenerate case the same way.

// normalize() of the zero vector divides by zero. A degenerate vertex
// normal (a zero normal in the asset, or a normal matrix of a collapsed
// object) would turn into NaN there and, interpolated, into NaN fragments.
// This returns the zero vector instead, which the lighting treats as a
// normal that receives no direct light: finite, and visible as such.
//
// Below the tiny threshold of dot(v, v) the result is shorter than unit
// length rather than exactly zero; the threshold (1e-30, a length of 1e-15)
// is far below any meaningful normal, including the output of a normal
// matrix of a very small object.
vec3 Safe_normalize(vec3 _v)
{
    return _v * inversesqrt(max(dot(_v, _v), 1.0e-30));
}

// True when no component is NaN or infinite. Only meaningful where the
// driver keeps NaN and infinity (every desktop Vulkan driver does for
// the operations used here).
bool Is_finite(vec3 _v)
{
    return !(any(isnan(_v)) || any(isinf(_v)));
}

bool Is_finite(vec4 _v)
{
    return !(any(isnan(_v)) || any(isinf(_v)));
}

#endif
