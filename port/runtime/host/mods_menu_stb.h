// The cards' pictures: stb_image's reader, PNG and JPEG only, told the size from
// the header before it decodes anything.
// (Each Mods menu file that decodes a picture includes this and has its own copy, STB_IMAGE_STATIC.)
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 4096
#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"
