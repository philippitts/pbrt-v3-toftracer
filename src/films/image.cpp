
/*
    pbrt source code is Copyright(c) 1998-2015
                        Matt Pharr, Greg Humphreys, and Wenzel Jakob.

    This file is part of pbrt.

    Redistribution and use in source and binary forms, with or without
    modification, are permitted provided that the following conditions are
    met:

    - Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.

    - Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
    IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
    TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A
    PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
    HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
    LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
    DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
    THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
    OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 */

#include "stdafx.h"

// core/film.cpp*
#include "image.h"
#include "paramset.h"
#include "imageio.h"
#include "stats.h"

// Film Method Definitions
ImageFilm::ImageFilm(const Point2i &resolution, const Bounds2f &cropWindow,
           std::unique_ptr<Filter> filt, Float diagonal,
           const std::string &filename, Float scale)
    : Film(resolution, cropWindow, std::move(filt), diagonal, filename, scale) {
	pixels = std::unique_ptr<Pixel[]>(new Pixel[croppedPixelBounds.Area()]);
}

std::unique_ptr<FilmTile> ImageFilm::GetFilmTile(const Bounds2i &sampleBounds) {
    // Bound image pixels that samples in _sampleBounds_ contribute to
    Vector2f halfPixel = Vector2f(0.5f, 0.5f);
    Bounds2f floatBounds = (Bounds2f)sampleBounds;
    Point2i p0 = (Point2i)Ceil(floatBounds.pMin - halfPixel - filter->radius);
    Point2i p1 = (Point2i)Floor(floatBounds.pMax - halfPixel + filter->radius) +
                 Point2i(1, 1);
    Bounds2i tilePixelBounds = Intersect(Bounds2i(p0, p1), croppedPixelBounds);
    return std::unique_ptr<FilmTile>(new ImageFilmTile(
        tilePixelBounds, filter->radius, filterTable, filterTableWidth));
}

void ImageFilm::MergeFilmTile(std::unique_ptr<FilmTile> tile) {
    ProfilePhase p(Prof::MergeFilmTile);
    std::lock_guard<std::mutex> lock(mutex);

	ImageFilmTile* imageTile = static_cast<ImageFilmTile*>(tile.get());
	if (imageTile == nullptr) {
		Warning("Skipping alien film tile in MergeFilmTile");
		return;
	}

    for (Point2i pixel : imageTile->GetPixelBounds()) {
        // Merge _pixel_ into _Film::pixels_
        const ImageTilePixel &tilePixel = imageTile->GetPixel(pixel);
        Pixel &mergePixel = GetPixel(pixel);
        Float xyz[3];
        tilePixel.contribSum.ToXYZ(xyz);
        for (int i = 0; i < 3; ++i) mergePixel.xyz[i] += xyz[i];
        mergePixel.filterWeightSum += tilePixel.filterWeightSum;
    }
}

void ImageFilm::SetImage(const Spectrum *img) const {
    int nPixels = croppedPixelBounds.Area();
    for (int i = 0; i < nPixels; ++i) {
        Pixel &p = pixels[i];
        img[i].ToXYZ(p.xyz);
        p.filterWeightSum = 1;
        p.splatXYZ[0] = p.splatXYZ[1] = p.splatXYZ[2] = 0;
    }
}

void ImageFilm::AddSplat(const Point2f &p, const IntegrationResult &v) {
    if (v.L.HasNaNs()) {
        Warning("Film ignoring splatted spectrum with NaN values");
        return;
    }
    ProfilePhase pp(Prof::SplatFilm);
    if (!InsideExclusive((Point2i)p, croppedPixelBounds)) return;
    Float xyz[3];
    v.L.ToXYZ(xyz);
    Pixel &pixel = GetPixel((Point2i)p);
    for (int i = 0; i < 3; ++i) pixel.splatXYZ[i].Add(xyz[i]);
}

void ImageFilm::WriteImage(Float splatScale) {
    // Convert image to RGB and compute final pixel values
    std::unique_ptr<Float[]> rgb(new Float[3 * croppedPixelBounds.Area()]);
    int offset = 0;
    for (Point2i p : croppedPixelBounds) {
        // Convert pixel XYZ color to RGB
        Pixel &pixel = GetPixel(p);
        XYZToRGB(pixel.xyz, &rgb[3 * offset]);

        // Normalize pixel with weight sum
        Float filterWeightSum = pixel.filterWeightSum;
        if (filterWeightSum != 0) {
            Float invWt = (Float)1 / filterWeightSum;
            rgb[3 * offset] = std::max((Float)0, rgb[3 * offset] * invWt);
            rgb[3 * offset + 1] =
                std::max((Float)0, rgb[3 * offset + 1] * invWt);
            rgb[3 * offset + 2] =
                std::max((Float)0, rgb[3 * offset + 2] * invWt);
        }
		
        // Add splat value at pixel
        Float splatRGB[3];
        Float splatXYZ[3] = {pixel.splatXYZ[0], pixel.splatXYZ[1],
                             pixel.splatXYZ[2]};
        XYZToRGB(splatXYZ, splatRGB);
        rgb[3 * offset] += splatScale * splatRGB[0];
        rgb[3 * offset + 1] += splatScale * splatRGB[1];
        rgb[3 * offset + 2] += splatScale * splatRGB[2];

        // Scale pixel value by _scale_
        rgb[3 * offset] *= scale;
        rgb[3 * offset + 1] *= scale;
        rgb[3 * offset + 2] *= scale;

        ++offset;
    }

    // Write RGB image
    ::WriteImage(filename, &rgb[0], croppedPixelBounds, fullResolution);
}

ImageFilm *CreateImageFilm(const ParamSet &params, std::unique_ptr<Filter> filter) {
    // Intentionally use FindOneString() rather than FindOneFilename() here
    // so that the rendered image is left in the working directory, rather
    // than the directory the scene file lives in.
    std::string filename = params.FindOneString("filename", "");
    if (PbrtOptions.imageFile != "") {
        if (filename != "") {
            Warning(
                "Output filename supplied on command line, \"%s\", ignored "
                "due to filename provided in scene description file, \"%s\".",
                PbrtOptions.imageFile.c_str(), filename.c_str());
        } else
            filename = PbrtOptions.imageFile;
    }
    if (filename == "") filename = "pbrt.exr";

    int xres = params.FindOneInt("xresolution", 1280);
    int yres = params.FindOneInt("yresolution", 720);
    if (PbrtOptions.quickRender) xres = std::max(1, xres / 4);
    if (PbrtOptions.quickRender) yres = std::max(1, yres / 4);
    Bounds2f crop(Point2f(0, 0), Point2f(1, 1));
    int cwi;
    const Float *cr = params.FindFloat("cropwindow", &cwi);
    if (cr && cwi == 4) {
        crop.pMin.x = Clamp(std::min(cr[0], cr[1]), 0.f, 1.f);
        crop.pMax.x = Clamp(std::max(cr[0], cr[1]), 0.f, 1.f);
        crop.pMin.y = Clamp(std::min(cr[2], cr[3]), 0.f, 1.f);
        crop.pMax.y = Clamp(std::max(cr[2], cr[3]), 0.f, 1.f);
    } else if (cr)
        Error("%d values supplied for \"cropwindow\". Expected 4.", cwi);

    Float scale = params.FindOneFloat("scale", 1.);
    Float diagonal = params.FindOneFloat("diagonal", 35.);

    return new ImageFilm(Point2i(xres, yres), crop, std::move(filter), diagonal,
                    filename, scale);
}

ImageFilmTile::ImageFilmTile(const Bounds2i &pixelBounds, const Vector2f &filterRadius,
	const Float *filterTable, int filterTableSize) :
	FilmTile(pixelBounds, filterRadius, filterTable, filterTableSize) {
	pixels = std::vector<ImageTilePixel>(std::max(0, pixelBounds.Area()));
}

void ImageFilmTile::AddSample(const Point2f &pFilm, 
	const IntegrationResult &integration, Float sampleWeight) {
	// Compute sample's raster bounds
	Point2f pFilmDiscrete = pFilm - Vector2f(0.5f, 0.5f);
	Point2i p0 = (Point2i)Ceil(pFilmDiscrete - filterRadius);
	Point2i p1 =
		(Point2i)Floor(pFilmDiscrete + filterRadius) + Point2i(1, 1);
	p0 = Max(p0, pixelBounds.pMin);
	p1 = Min(p1, pixelBounds.pMax);

	// Loop over filter support and add sample to pixel arrays

	// Precompute $x$ and $y$ filter table offsets
	int *ifx = ALLOCA(int, p1.x - p0.x);
	for (int x = p0.x; x < p1.x; ++x) {
		Float fx = std::abs((x - pFilmDiscrete.x) * invFilterRadius.x *
			filterTableSize);
		ifx[x - p0.x] = std::min((int)std::floor(fx), filterTableSize - 1);
	}
	int *ify = ALLOCA(int, p1.y - p0.y);
	for (int y = p0.y; y < p1.y; ++y) {
		Float fy = std::abs((y - pFilmDiscrete.y) * invFilterRadius.y *
			filterTableSize);
		ify[y - p0.y] = std::min((int)std::floor(fy), filterTableSize - 1);
	}
	for (int y = p0.y; y < p1.y; ++y) {
		for (int x = p0.x; x < p1.x; ++x) {
			// Evaluate filter value at $(x,y)$ pixel
			int offset = ify[y - p0.y] * filterTableSize + ifx[x - p0.x];
			Float filterWeight = filterTable[offset];

			// Update pixel values with filtered sample contribution
			ImageTilePixel &pixel = GetPixel(Point2i(x, y));
			pixel.contribSum += integration.L * sampleWeight * filterWeight;
			pixel.filterWeightSum += filterWeight;
		}
	}
}

ImageTilePixel &ImageFilmTile::GetPixel(const Point2i &p) {
	Assert(InsideExclusive(p, pixelBounds));
	int width = pixelBounds.pMax.x - pixelBounds.pMin.x;
	int offset =
		(p.x - pixelBounds.pMin.x) + (p.y - pixelBounds.pMin.y) * width;
	return pixels[offset];
}