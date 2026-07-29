#ifndef __TRACYVERSION_HPP__
#define __TRACYVERSION_HPP__

namespace tracy
{
// We have a different file format from upstream so I chnage the code to (hopefully) force a conflict if
// the file format changes. Make sure update the file format number with the diverging offset we have.
// Therefore we always need to stay ahead of the upstream file format but also make sure we don't inadvertently
// end up with a version we have already "burned/used up".
// When making changes to our file format, increase the "offset" value, when upstream changes the file format simply add
// our current offset value!
//
// Update the comment when updating the file format!
// Upstream patch file format version: 0
// Our current offset from upstream is: 1
//
enum : uint32_t { ForceFileVersionPatchConflict = 0 };

namespace Version
{
enum { Major = 0 };
enum { Minor = 12 };
enum { Patch = ForceFileVersionPatchConflict + 1 };
}
}

#endif
