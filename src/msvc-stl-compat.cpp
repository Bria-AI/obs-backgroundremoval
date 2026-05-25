// Stub definitions for MSVC STL vectorised search helpers that are referenced
// by opencv_core's log-tag configuration parser when built with MSVC 14.40+,
// but are not present as a link-time symbol in any shipped MSVC static library
// (they normally live in msvcp140_2.dll which has no corresponding .lib).
//
// The real implementations use SIMD; these fallbacks are scalar and are only
// ever called from OpenCV's startup log-tag parsing — not from hot paths.

#ifdef _MSC_VER

#include <climits>
#include <cstddef>

extern "C" {

// Returns the 0-based index of the first byte in haystack[0..hay_size) that is
// NOT present in needle[0..needle_size).  Returns SIZE_MAX if every byte matches.
size_t __std_find_first_not_of_trivial_pos_1(const void *haystack, size_t hay_size, const void *needle,
					     size_t needle_size) noexcept
{
	const auto *hs = static_cast<const unsigned char *>(haystack);
	const auto *nd = static_cast<const unsigned char *>(needle);

	for (size_t i = 0; i < hay_size; ++i) {
		bool found = false;
		for (size_t j = 0; j < needle_size; ++j) {
			if (hs[i] == nd[j]) {
				found = true;
				break;
			}
		}
		if (!found)
			return i;
	}
	return SIZE_MAX;
}

// Returns the 0-based index of the last byte in haystack[0..hay_size) that is
// NOT present in needle[0..needle_size).  Returns SIZE_MAX if every byte matches.
size_t __std_find_last_not_of_trivial_pos_1(const void *haystack, size_t hay_size, const void *needle,
					    size_t needle_size) noexcept
{
	const auto *hs = static_cast<const unsigned char *>(haystack);
	const auto *nd = static_cast<const unsigned char *>(needle);

	for (size_t i = hay_size; i > 0; --i) {
		bool found = false;
		for (size_t j = 0; j < needle_size; ++j) {
			if (hs[i - 1] == nd[j]) {
				found = true;
				break;
			}
		}
		if (!found)
			return i - 1;
	}
	return SIZE_MAX;
}

} // extern "C"

#endif // _MSC_VER
