// #28 monolith split — dedicated single-header stb implementation TU.
//
// The VulkanRenderDevice methods writeCapturePng()/captureFrame() (PNG screenshot)
// and bakeTtfAtlas()/buildFontAtlas() (HUD glyph atlas) call into stb_image_write
// and stb_truetype. Those single-header libs must have their IMPLEMENTATION
// compiled in exactly ONE translation unit; this is that TU. The shared internal
// header includes the same stb headers WITHOUT the impl macros (declarations only),
// so any vk/*.cpp that calls the stb API links against the symbols defined here.
//
// ModelLoader.cpp owns the matching STB_IMAGE_IMPLEMENTATION (the reader) — a
// separate stb header, so no symbol clash.
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include <stb_truetype.h>
