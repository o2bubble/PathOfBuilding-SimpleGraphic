// SimpleGraphic Engine
// (c) David Gowor, 2014
//
// Module: Render Font
//

#include "r_local.h"

#include <fmt/format.h>
#include <iostream>
#include <fstream>
#include <string>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_SYNTHESIS_H
#include FT_TRUETYPE_TABLES_H

#include "r_font_fallback.h"

// =======
// Classes
// =======

// Glyph parameters
struct f_glyph_s {
	float	tcLeft = 0.0;
	float	tcRight = 0.0;
	float	tcTop = 0.0;
	float	tcBottom = 0.0;
	int		width = 0;
	int		spLeft = 0;
	int		spRight = 0;
};

// Font height info
struct f_fontHeight_s {
	r_tex_c* tex;
	int		height;
	int		numGlyph;
	f_glyph_s glyphs[128];
	f_glyph_s defGlyph{0.0f, 0.0f, 0.0f, 0.0f, 0, 0, 0};

	f_glyph_s const& Glyph(char ch) const {
		if ((unsigned char)ch >= numGlyph) {
			return defGlyph;
		}
		return glyphs[(unsigned char)ch];
	}
};

// =====================
// FreeType CJK Fallback
// =====================

static const int FT_ATLAS_SIZE = 2048;

// Glyphs are rasterized at the FINAL drawn pixel height, not at the bitmap
// bucket height and then rescaled: rescaling a grayscale bitmap under
// GL_LINEAR is what smears CJK text. Draw and measure must map a text height
// through the same helper or advances drift apart.
static inline int FtPixelSize(int height) {
	return height > 0 ? height : 1;
}

struct FtCachedGlyph {
	int atlasIndex = -1;
	float tcLeft = 0.0f, tcRight = 0.0f, tcTop = 0.0f, tcBottom = 0.0f;
	int bitmapWidth = 0;
	int bitmapRows = 0;
	int bearingX = 0;
	int bearingY = 0;
	int advance = 0; // pixels
};

struct FtAtlas {
	r_tex_c* tex = nullptr;
	int curX = 1; // 1px padding from the edge, so GL_LINEAR never samples a neighbour
	int curY = 1;
	int rowHeight = 0;
};

struct r_font_c::FtCache {
	FT_Library library = nullptr;
	FT_Face face = nullptr;
	r_renderer_c* renderer = nullptr;
	std::vector<FtAtlas> atlases;
	// Key: (pixelSize << 21) | codepoint — heights up to 2048, codepoints to 2M.
	std::unordered_map<uint64_t, FtCachedGlyph> cache;
	// Codepoints the face has no glyph for, so no atlas slot is burned twice.
	std::unordered_set<char32_t> missing;
	// Other shipped faces, tried in order when the primary lacks a codepoint.
	FtFallbackChain fallback;
	// Baseline as a fraction of the line cell, from the PRIMARY face's metrics.
	float baselineRatio = 0.82f;

	bool Init(r_renderer_c* rend, const std::string& ttfPath) {
		renderer = rend;
		if (FT_Init_FreeType(&library)) {
			rend->sys->con->Warning("FreeType: failed to initialize library");
			return false;
		}
		if (FT_New_Face(library, ttfPath.c_str(), 0, &face)) {
			rend->sys->con->Warning("FreeType: failed to load font '%s'", ttfPath.c_str());
			FT_Done_FreeType(library);
			library = nullptr;
			return false;
		}
		rend->sys->con->Printf("FreeType: loaded CJK fallback font '%s'", ttfPath.c_str());

		// Baseline from the face's real metrics rather than the 0.82 guess.
		// OS/2 typo ascender/descender puts the ideographic em box flush with
		// the line cell for CJK faces; hhea is the fallback.
		long asc = 0, desc = 0;
		if (auto* os2 = (TT_OS2*)FT_Get_Sfnt_Table(face, FT_SFNT_OS2)) {
			if (os2->sTypoAscender > 0 && os2->sTypoAscender > os2->sTypoDescender) {
				asc = os2->sTypoAscender;
				desc = os2->sTypoDescender;
			}
		}
		if (!asc && face->ascender > 0 && face->ascender > face->descender) {
			asc = face->ascender;
			desc = face->descender;
		}
		if (asc) {
			float ratio = (float)asc / (float)(asc - desc);
			if (ratio >= 0.5f && ratio <= 1.0f) baselineRatio = ratio;
		}
		return true;
	}

	~FtCache() {
		for (auto& atlas : atlases) delete atlas.tex;
		fallback.Close();
		if (face) FT_Done_Face(face);
		if (library) FT_Done_FreeType(library);
	}

	FT_Face FaceFor(char32_t cp) {
		return fallback.FaceFor(library, face, cp, [this, cp](const std::string& path, bool ok) {
			if (ok) {
				renderer->sys->con->Printf(
					"FreeType: opened fallback font '%s' (U+%04X missing from the primary face)",
					path.c_str(), (unsigned)cp);
			} else {
				renderer->sys->con->Warning("FreeType: failed to open fallback font '%s'", path.c_str());
			}
		});
	}

	FtAtlas& EnsureAtlas(int glyphW, int glyphH) {
		if (!atlases.empty()) {
			auto& last = atlases.back();
			if (last.curX + glyphW + 1 <= FT_ATLAS_SIZE &&
			    last.curY + (std::max)(last.rowHeight, glyphH) + 1 <= FT_ATLAS_SIZE) {
				return last;
			}
			if (last.curY + last.rowHeight + glyphH + 2 <= FT_ATLAS_SIZE) {
				last.curX = 1;
				last.curY += last.rowHeight + 1;
				last.rowHeight = 0;
				return last;
			}
		}

		FtAtlas atlas;
		auto img = std::make_unique<image_c>();
		std::vector<byte> zeros(FT_ATLAS_SIZE * FT_ATLAS_SIZE * 4, 0);
		img->CopyRaw(IMGTYPE_RGBA, FT_ATLAS_SIZE, FT_ATLAS_SIZE, zeros.data());
		atlas.tex = new r_tex_c(renderer->texMan, std::move(img), TF_NOMIPMAP | TF_CLAMP);
		atlas.tex->fileWidth = FT_ATLAS_SIZE;
		atlas.tex->fileHeight = FT_ATLAS_SIZE;
		atlases.push_back(atlas);

		renderer->sys->con->Printf("FreeType: created atlas #%d (%dx%d)",
			(int)atlases.size(), FT_ATLAS_SIZE, FT_ATLAS_SIZE);
		return atlases.back();
	}

	// Returns null when the face (and every fallback) has no glyph for cp, so
	// the caller can draw the [U+XXXX] placeholder instead of an empty box.
	FtCachedGlyph* GetGlyph(char32_t cp, int pixelSize) {
		if (!face) return nullptr;

		uint64_t key = ((uint64_t)pixelSize << 21) | (uint32_t)cp;
		auto it = cache.find(key);
		if (it != cache.end()) return &it->second;
		if (missing.count(cp)) return nullptr;

		FT_Face useFace = FaceFor(cp);
		if (FT_Get_Char_Index(useFace, (FT_ULong)cp) == 0) {
			missing.insert(cp);
			return nullptr;
		}

		FT_Set_Pixel_Sizes(useFace, 0, pixelSize);
		if (FT_Load_Char(useFace, (FT_ULong)cp, FT_LOAD_RENDER)) return nullptr;

		FT_GlyphSlot slot = useFace->glyph;
		FT_Bitmap& bmp = slot->bitmap;
		int glyphW = (int)bmp.width;
		int glyphH = (int)bmp.rows;

		// Zero-size glyphs (spaces in CJK) still advance the pen.
		if (glyphW == 0 || glyphH == 0) {
			FtCachedGlyph glyph{};
			glyph.bearingX = slot->bitmap_left;
			glyph.bearingY = slot->bitmap_top;
			glyph.advance = (int)(slot->advance.x >> 6);
			auto [inserted, _] = cache.emplace(key, glyph);
			return &inserted->second;
		}

		FtAtlas& atlas = EnsureAtlas(glyphW, glyphH);
		int atlasIdx = (int)(atlases.size() - 1);
		int x = atlas.curX;
		int y = atlas.curY;

		std::vector<byte> pixels(glyphW * glyphH * 4);
		for (int row = 0; row < glyphH; row++) {
			for (int col = 0; col < glyphW; col++) {
				int srcIdx = row * bmp.pitch + col;
				int dstIdx = (row * glyphW + col) * 4;
				byte gray = bmp.num_grays > 1 ? bmp.buffer[srcIdx] : 255;
				pixels[dstIdx + 0] = 255;
				pixels[dstIdx + 1] = 255;
				pixels[dstIdx + 2] = 255;
				pixels[dstIdx + 3] = gray; // coverage in alpha; the quad tints it
			}
		}

		glBindTexture(atlas.tex->target, atlas.tex->texId);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		if (atlas.tex->target == GL_TEXTURE_2D_ARRAY) {
			glTexSubImage3D(atlas.tex->target, 0, x, y, 0,
				glyphW, glyphH, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
		} else {
			glTexSubImage2D(atlas.tex->target, 0, x, y,
				glyphW, glyphH, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
		}

		FtCachedGlyph glyph{};
		glyph.atlasIndex = atlasIdx;
		glyph.tcLeft = (float)x / FT_ATLAS_SIZE;
		glyph.tcRight = (float)(x + glyphW) / FT_ATLAS_SIZE;
		glyph.tcTop = (float)y / FT_ATLAS_SIZE;
		glyph.tcBottom = (float)(y + glyphH) / FT_ATLAS_SIZE;
		glyph.bitmapWidth = glyphW;
		glyph.bitmapRows = glyphH;
		glyph.bearingX = slot->bitmap_left;
		glyph.bearingY = slot->bitmap_top;
		glyph.advance = (int)(slot->advance.x >> 6); // FT advances are 26.6 fixed point

		auto [inserted, _] = cache.emplace(key, glyph);
		atlas.curX += glyphW + 1;
		atlas.rowHeight = (std::max)(atlas.rowHeight, glyphH);
		return &inserted->second;
	}
};

// ===========
// Font Loader
// ===========

r_font_c::r_font_c(r_renderer_c* renderer, const char* fontName)
	: renderer(renderer)
{
	numFontHeight = 0;
	fontHeightMap = NULL;

	std::string fileNameBase = fmt::format(CFG_DATAPATH "Fonts/{}", fontName);

	// Open info file
	std::string tgfName = fileNameBase + ".tgf";
	std::ifstream tgf(tgfName);
	if (!tgf) {
		renderer->sys->con->Warning("font \"%s\" not found", fontName);
		return;
	}

	maxHeight = 0;
	f_fontHeight_s* fh = NULL;

	// Parse info file
	std::string sub;
	while (std::getline(tgf, sub)) {
		int h, x, y, w, sl, sr;
		if (sscanf(sub.c_str(), "HEIGHT %u;", &h) == 1) {
			// New height
			fh = new f_fontHeight_s;
			fontHeights[numFontHeight++] = fh;
			std::string tgaName = fmt::format("{}.{}.tga", fileNameBase, h);
			fh->tex = new r_tex_c(renderer->texMan, tgaName.c_str(), TF_NOMIPMAP);
			fh->height = h;
			if (h > maxHeight) {
				maxHeight = h;
			}
			fh->numGlyph = 0;
		}
		else if (fh && sscanf(sub.c_str(), "GLYPH %u %u %u %d %d;", &x, &y, &w, &sl, &sr) == 5) {
			// Add glyph
			if (fh->numGlyph >= 128) continue;
			f_glyph_s* glyph = &fh->glyphs[fh->numGlyph++];
			glyph->tcLeft = (float)x / fh->tex->fileWidth;
			glyph->tcRight = (float)(x + w) / fh->tex->fileWidth;
			glyph->tcTop = (float)y / fh->tex->fileHeight;
			glyph->tcBottom = (float)(y + fh->height) / fh->tex->fileHeight;
			glyph->width = w;
			glyph->spLeft = sl;
			glyph->spRight = sr;
		}
	}

	// Generate mapping of text height to font height
	fontHeightMap = new int[maxHeight + 1];
	memset(fontHeightMap, 0, sizeof(int) * (maxHeight + 1));
	for (int i = 0; i < numFontHeight; i++) {
		int gh = fontHeights[i]->height;
		for (int h = gh; h <= maxHeight; h++) {
			fontHeightMap[h] = i;
		}
		if (i > 0) {
			int belowH = fontHeights[i - 1]->height;
			int lim = (gh - belowH - 1) / 2;
			for (int b = 0; b < lim; b++) {
				fontHeightMap[gh - b - 1] = i;
			}
		}
	}

	// Try to initialize the FreeType CJK fallback. Search order:
	//   1. POB_ZH_FONTDIR/Fonts/<POB_ZH_FONTFILE>  (explicit user choice)
	//   2. POB_ZH_FONTDIR/Fonts/NotoSansTC-Regular.ttf, then FZ_ZY.ttf
	//   3. SimpleGraphic/Fonts/<same names>        (CWD = the POB folder)
	std::vector<std::string> ttfCandidates;
	auto addCandidate = [&](const std::string& dir, const std::string& file) {
		if (!dir.empty() && file.size()) {
			std::string p = dir;
			if (p.back() != '/' && p.back() != '\\') p += '/';
			ttfCandidates.push_back(p + file);
		}
	};
	// 1. Explicit override: POB_ZH_FONTDIR points at a folder holding Fonts/.
	if (const char* dir = getenv("POB_ZH_FONTDIR")) {
		if (const char* ff = getenv("POB_ZH_FONTFILE")) {
			if (ff[0]) addCandidate(std::string(dir) + "/Fonts", ff);
		}
		addCandidate(std::string(dir) + "/Fonts", "NotoSansTC-Regular.ttf");
		addCandidate(std::string(dir) + "/Fonts", "FZ_ZY.ttf");
	}
	// 2. Next to the shipped bitmap fonts, resolved from the exe directory —
	//    the CWD is the script folder by the time fonts load, so a relative
	//    CFG_DATAPATH alone would never find them.
	const std::string exeDir = renderer->sys->basePath.string();
	addCandidate(exeDir + "/" + CFG_DATAPATH "Fonts", "NotoSansTC-Regular.ttf");
	addCandidate(exeDir + "/" + CFG_DATAPATH "Fonts", "FZ_ZY.ttf");
	addCandidate(exeDir + "/" + CFG_DATAPATH "Fonts", "CJKFallback.ttf");
	// 3. runtime/Fonts/ and CWD-relative fallbacks.
	addCandidate(exeDir + "/Fonts", "NotoSansTC-Regular.ttf");
	addCandidate(exeDir + "/Fonts", "FZ_ZY.ttf");
	addCandidate("Fonts", "NotoSansTC-Regular.ttf");
	addCandidate("Fonts", "FZ_ZY.ttf");
	addCandidate(CFG_DATAPATH "Fonts", "NotoSansTC-Regular.ttf");
	addCandidate(CFG_DATAPATH "Fonts", "FZ_ZY.ttf");

	for (size_t i = 0; i < ttfCandidates.size(); i++) {
		std::ifstream probe(ttfCandidates[i]);
		if (!probe.good()) continue;
		probe.close();
		auto cache = std::make_unique<FtCache>();
		if (cache->Init(renderer, ttfCandidates[i])) {
			// Every other candidate that exists becomes a fallback, in the
			// same priority order: Noto Sans TC is a subset build and lacks
			// characters the game's own text uses.
			cache->fallback.CollectFallbacks(ttfCandidates, i, [](const std::string& path) {
				std::ifstream p(path);
				return p.good();
			});
			ftCache = std::move(cache);
			break;
		}
	}
}

r_font_c::~r_font_c()
{
	// Delete textures
	for (int i = 0; i < numFontHeight; i++) {
		delete fontHeights[i]->tex;
		delete fontHeights[i];
	}
	delete fontHeightMap;
}

// =============
// Font Renderer
// =============

std::u32string BuildTofuString(char32_t cp) {
	// Format unhandled Unicode codepoints like U+0123, higher planes have wider numbers.
	fmt::memory_buffer buf;
	fmt::format_to(fmt::appender(buf), "[U+{:04X}]", (uint32_t)cp);
	std::u32string ret;
	ret.reserve(buf.size());
	std::copy(buf.begin(), buf.end(), std::back_inserter(ret));
	return ret;
}

int const tofuSizeReduction = 3;

int r_font_c::StringWidthInternal(f_fontHeight_s* fh, std::u32string_view str, int height, float scale)
{
	int heightIdx = (int)(std::find(fontHeights, fontHeights + numFontHeight, fh) - fontHeights);
	auto tofuFont = FindSmallerFontHeight(height, heightIdx, tofuSizeReduction);

	auto measureCodepoint = [](f_fontHeight_s* glyphFh, char32_t cp) {
		auto& glyph = glyphFh->Glyph((char)(unsigned char)cp);
		return glyph.width + glyph.spLeft + glyph.spRight;
	};

	float width = 0.0f;
	for (size_t idx = 0; idx < str.size();) {
		auto ch = str[idx];
		int escLen = IsColorEscape(str.substr(idx));
		if (escLen) {
			idx += escLen;
		}
		else if (ch >= (unsigned)fh->numGlyph) {
			// CJK: measure with the same FreeType advance the draw path uses,
			// or the caret and the selection background drift off the glyphs.
			if (ftCache) {
				if (auto* ftGlyph = ftCache->GetGlyph(ch, FtPixelSize(height))) {
					width += ftGlyph->advance;
					width = std::ceil(width);
					++idx;
					continue;
				}
			}
			auto tofu = BuildTofuString(ch);
			for (auto cp : tofu) {
				width += measureCodepoint(tofuFont.fh, cp);
				width = std::ceil(width);
			}
			++idx;
		}
		else if (ch == U'\t') {
			auto& glyph = fh->Glyph(' ');
			int spWidth = glyph.width + glyph.spLeft + glyph.spRight;
			width += spWidth * 4 * scale;
			width = std::ceil(width);
			++idx;
		}
		else {
			width += measureCodepoint(fh, ch) * scale;
			width = std::ceil(width);
			++idx;
		}
	}
	return static_cast<int>(width);
}

int r_font_c::StringWidth(int height, std::u32string_view str)
{
	auto mainFont = FindFontHeight(height);
	f_fontHeight_s* fh = mainFont.fh;
	int max = 0;
	float const scale = (float)height / fh->height;
	for (auto I = str.begin(); I != str.end(); ++I) {
		auto lineEnd = std::find(I, str.end(), U'\n');
		if (I != lineEnd) {
			std::u32string_view line(&*I, std::distance(I, lineEnd));
			int lw = StringWidthInternal(fh, line, height, scale);
			max = (std::max)(max, lw);
		}
		if (lineEnd == str.end()) {
			break;
		}
		I = lineEnd;
	}
	return max;
}

size_t r_font_c::StringCursorInternal(f_fontHeight_s* fh, std::u32string_view str, int height, float scale, int curX)
{
	int heightIdx = (int)(std::find(fontHeights, fontHeights + numFontHeight, fh) - fontHeights);
	auto tofuFont = FindSmallerFontHeight(height, heightIdx, tofuSizeReduction);

	auto measureCodepoint = [](f_fontHeight_s* glyphFh, char32_t cp) {
		auto& glyph = glyphFh->Glyph((char)(unsigned char)cp);
		return glyph.width + glyph.spLeft + glyph.spRight;
	};

	float x = 0.0f;
	auto I = str.begin();
	auto lineEnd = std::find(I, str.end(), U'\n');
	while (I != lineEnd) {
		auto tail = str.substr(std::distance(str.begin(), I));
		int escLen = IsColorEscape(tail);
		if (escLen) {
			I += escLen;
		}
		else if (*I >= (unsigned)fh->numGlyph) {
			// CJK: one codepoint is one caret stop, using the FreeType advance
			// that StringWidthInternal measures with.
			if (ftCache) {
				if (auto* ftGlyph = ftCache->GetGlyph(*I, FtPixelSize(height))) {
					x += ftGlyph->advance;
					x = std::ceil(x);
					if (curX <= x) {
						return std::distance(str.begin(), I);
					}
					++I;
					continue;
				}
			}
			auto tofu = BuildTofuString(*I);
			for (auto cp : tofu) {
				x += measureCodepoint(tofuFont.fh, cp);
				x = std::ceil(x);
				if (curX <= x) {
					return std::distance(str.begin(), I);
				}
			}
			++I;
		}
		else if (*I == U'\t') {
			auto& glyph = fh->Glyph(' ');
			float fullWidth = (glyph.width + glyph.spLeft + glyph.spRight) * 4.0f * scale;
			float halfWidth = std::ceil(fullWidth / 2.0f);
			x += halfWidth;
			x = std::ceil(x);
			if (curX <= x) {
				break;
			}
			x += fullWidth - halfWidth;
			x = std::ceil(x);
			if (curX <= x) {
				break;
			}
			++I;
		}
		else {
			x += measureCodepoint(fh, *I) * scale;
			x = std::ceil(x);
			if (curX <= x) {
				break;
			}
			++I;
		}
	}
	return std::distance(str.begin(), I);
}

int	r_font_c::StringCursorIndex(int height, std::u32string_view str, int curX, int curY)
{
	auto mainFont = FindFontHeight(height);
	f_fontHeight_s* fh = mainFont.fh;
	int lastIndex = 0;
	int lineY = height;
	float scale = (float)height / fh->height;

	auto I = str.begin();
	while (I != str.end()) {
		auto lineEnd = std::find(I, str.end(), U'\n');
		auto line = str.substr(std::distance(str.begin(), I), std::distance(I, lineEnd));
		lastIndex = (int)(StringCursorInternal(fh, line, height, scale, curX));
		if (curY <= lineY) {
			break;
		}
		if (lineEnd == str.end()) {
			break;
		}
		I = lineEnd + 1;
		lineY += height;
	}
	return (int)std::distance(str.begin(), I) + lastIndex;
}

r_font_c::EmbeddedFontSpec r_font_c::FindSmallerFontHeight(int height, int heightIdx, int sizeReduction) {
	EmbeddedFontSpec ret{};
	ret.fh = fontHeights[heightIdx];
	ret.yPad = 0;
	for (int tofuIdx = heightIdx - 1; tofuIdx >= 0; --tofuIdx) {
		auto candFh = fontHeights[tofuIdx];
		int heightDiff = height - candFh->height;
		if (heightDiff >= sizeReduction) {
			ret.fh = candFh;
			ret.yPad = (int)std::ceil(heightDiff / 2.0f);
			break;
		}
	}
	return ret;
}

r_font_c::FontHeightEntry r_font_c::FindFontHeight(int height) {
	FontHeightEntry ret{};
	if (height > maxHeight) {
		// Too large heights get the largest font size.
		ret.heightIdx = numFontHeight - 1;
	}
	else if (height < 0) {
		// Negative heights get the smallest font size.
		ret.heightIdx = 0;
	}
	else {
		ret.heightIdx = fontHeightMap[height];
	}
	ret.fh = fontHeights[ret.heightIdx];
	return ret;
}

void r_font_c::DrawTextLine(scp_t pos, int align, int height, col4_t col, std::u32string_view str)
{
	// Check if the line is visible
	if (pos[Y] >= renderer->sys->video->vid.size[1] || pos[Y] <= -height) {
		// Just process the colour codes
		while (!str.empty()) {
			// Check for escape character
			int escLen = IsColorEscape(str);
			if (escLen) {
				str = ReadColorEscape(str, col);
				col[3] = 1.0f;
				renderer->curLayer->Color(col);
				continue;
			}
			str = str.substr(1);
		}
		return;
	}

	// Find best height to use
	auto mainFont = FindFontHeight(height);
	f_fontHeight_s* fh = mainFont.fh;
	float scale = (float)height / fh->height;
	auto tofuFont = FindSmallerFontHeight(height, mainFont.heightIdx, tofuSizeReduction);

	// Calculate the string position
	float x = pos[X];
	float y = std::floor(pos[Y]);
	if (align != F_LEFT) {
		// Calculate the real width of the string
		float width = StringWidthInternal(fh, str, height, scale);
		switch (align) {
		case F_CENTRE:
			x = floor((renderer->VirtualScreenWidth() - width) / 2.0f + pos[X]);
			break;
		case F_RIGHT:
			x = floor(renderer->VirtualScreenWidth() - width - pos[X]);
			break;
		case F_CENTRE_X:
			x = floor(pos[X] - width / 2.0f);
			break;
		case F_RIGHT_X:
			x = floor(pos[X] - width);
			break;
		}
	}

	// Snap the starting x position to the pixel grid so the leading glyph isn't blurred.
	x = std::round(x);

	r_tex_c* curTex{};

	auto drawCodepoint = [this, &curTex, &x, y](f_fontHeight_s* fh, int height, float scale, int yShift, char32_t cp) {
		float cpY = y + yShift;
		if (curTex != fh->tex) {
			curTex = fh->tex;
			renderer->curLayer->Bind(fh->tex);
		}
		auto& glyph = fh->Glyph((char)(unsigned char)cp);
		x += glyph.spLeft * scale;
		if (glyph.width) {
			float w = glyph.width * scale;
			if (x + w >= 0 && x < renderer->VirtualScreenWidth()) {
				renderer->curLayer->Quad(
					glyph.tcLeft, glyph.tcTop, x, cpY,
					glyph.tcRight, glyph.tcTop, x + w, cpY,
					glyph.tcRight, glyph.tcBottom, x + w, cpY + height,
					glyph.tcLeft, glyph.tcBottom, x, cpY + height
				);
			}
			x += w;
		}
		x += glyph.spRight * scale;
		x = std::ceil(x);
	};

	// FreeType path. The glyph is rasterized at the final pixel height, and x,
	// bearings and bitmap sizes are integral, so the quad samples the atlas
	// texel-exact under GL_LINEAR — that is what keeps CJK text sharp.
	const int ftPx = FtPixelSize(height);
	const int ftBaselinePx = ftCache ? (int)std::lround(height * ftCache->baselineRatio) : 0;

	auto drawFtGlyph = [this, &curTex, &x, y, ftBaselinePx](FtCachedGlyph* ftg) {
		if (ftg->atlasIndex < 0 || ftg->bitmapWidth == 0) {
			x += ftg->advance; // space-like glyph: advance only
			x = std::ceil(x);
			return;
		}
		auto* atlasTex = ftCache->atlases[ftg->atlasIndex].tex;
		if (curTex != atlasTex) {
			curTex = atlasTex;
			renderer->curLayer->Bind(atlasTex);
		}
		float glyphX = std::round(x) + ftg->bearingX;
		float glyphY = y + ftBaselinePx - ftg->bearingY;
		float glyphW = (float)ftg->bitmapWidth;
		float glyphH = (float)ftg->bitmapRows;
		if (glyphX + glyphW >= 0 && glyphX < renderer->VirtualScreenWidth()) {
			renderer->curLayer->Quad(
				ftg->tcLeft, ftg->tcTop, glyphX, glyphY,
				ftg->tcRight, ftg->tcTop, glyphX + glyphW, glyphY,
				ftg->tcRight, ftg->tcBottom, glyphX + glyphW, glyphY + glyphH,
				ftg->tcLeft, ftg->tcBottom, glyphX, glyphY + glyphH
			);
		}
		x += ftg->advance;
		x = std::ceil(x);
	};

	// Render the string
	for (auto tail = str; !tail.empty();) {
		// Draw unprintable characters as tofu placeholders
		auto ch = tail[0];
		if (ch >= (unsigned)fh->numGlyph) {
			// CJK through FreeType; the [U+XXXX] placeholder is what is left
			// when no shipped face has the glyph either.
			if (ftCache) {
				if (auto* ftGlyph = ftCache->GetGlyph(ch, ftPx)) {
					drawFtGlyph(ftGlyph);
					tail = tail.substr(1);
					continue;
				}
			}
			auto tofu = BuildTofuString(ch);
			for (auto ch : tofu) {
				drawCodepoint(tofuFont.fh, tofuFont.fh->height, 1.0f, tofuFont.yPad, ch);
			}
			tail = tail.substr(1);
			continue;
		}

		// Check for escape character
		int escLen = IsColorEscape(tail);
		if (escLen) {
			tail = ReadColorEscape(tail, col);
			col[3] = 1.0f;
			renderer->curLayer->Color(col);
			continue;
		}

		// Handle tabs
		if (ch == U'\t') {
			auto& glyph = fh->Glyph(' ');
			int spWidth = glyph.width + glyph.spLeft + glyph.spRight;
			x+= (spWidth << 2) * scale;
			tail = tail.substr(1);
			continue;
		}

		// Draw glyph
		drawCodepoint(fh, height, scale, 0, ch);
		tail = tail.substr(1);
	}
}

void r_font_c::Draw(scp_t pos, int align, int height, col4_t col, std::u32string_view str)
{
	if (str.empty()) {
		pos[Y]+= height;
		return;
	}

	// Prepare for rendering
	renderer->curLayer->Color(col);

	// Separate into lines and render them
	for (auto I = str.begin(); I != str.end(); ++I) {
		auto lineEnd = std::find(I, str.end(), U'\n');
		if (I != lineEnd) {
			std::u32string_view line(&*I, std::distance(I, lineEnd));
			DrawTextLine(pos, align, height, col, line);
		}
		pos[Y] += height;
		if (lineEnd == str.end()) {
			break;
		}
		I = lineEnd;
	}
}

void r_font_c::FDraw(scp_t pos, int align, int height, col4_t col, const char* fmt, ...)
{
	va_list va;
	va_start(va, fmt);
	VDraw(pos, align, height, col, fmt, va);
	va_end(va);
}

void r_font_c::VDraw(scp_t pos, int align, int height, col4_t col, const char* fmt, va_list va)
{
	char str[65536];
	vsnprintf(str, 65535, fmt, va);
	str[65535] = 0;
	auto idxStr = IndexUTF8ToUTF32(str);
	Draw(pos, align, height, col, idxStr.text);
}
