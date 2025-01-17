
/*
This file is part of the Time-of-Flight Tracer program. It is not part of
the original PBRT source distribution. See the included license file for
more information.

Copyright(c) 2016 Microsoft Corporation

Author: Phil Pitts
*/

#if defined(_MSC_VER)
#define NOMINMAX
#pragma once
#endif

#ifndef PBRT_FILMS_HISTOGRAMFILM_H
#define PBRT_FILMS_HISTOGRAMFILM_H
#include "stdafx.h"

// films/histogramfilm.h*
#include "pbrt.h"
#include "film.h"
#include "histogram.h"
#include "paramset.h"

// HistogramTilePixel Declarations
class HistogramTilePixel {
public:
	// HistogramTilePixel Public Methods
	HistogramTilePixel() { filterWeightSum = 0.f; }
	void Initialize(Float binSize, Float maxPathLength) {
		histogram = Histogram(binSize, maxPathLength);
	}

	// HistogramTilePixel Public Members
	Histogram histogram;
	Float filterWeightSum;
};

// HistogramFilmTile Declarations
class HistogramFilmTile : public FilmTile {
public:
	// HistogramFilmTile Public Methods
	HistogramFilmTile(const Bounds2i &pixelBounds, const Vector2f &filterRadius,
		const Float *filterTable, int filterTableSize, Float binSize, Float maxDistance);
	void AddSample(const Point2f &pFilm, const IntegrationResult &integration,
		Float sampleWeight);
	HistogramTilePixel &GetPixel(const Point2i &p);

private:
	// HistogramFilmTile Private Methods
	std::vector<HistogramTilePixel> pixels;
};

// HistogramFilm Declarations
class HistogramFilm : public Film {
public:
	// HistogramFilm Public Methods
	HistogramFilm(const Point2i &resolution, const Bounds2f &cropWindow,
		std::unique_ptr<Filter> filter, Float diagonal,
		const std::string &filename, Float scale, Float binSize, 
		Float maxPathLength, Float minL);

	std::unique_ptr<FilmTile> GetFilmTile(const Bounds2i &sampleBounds);
	void MergeFilmTile(std::unique_ptr<FilmTile> tile);
	void SetImage(const Spectrum *img) const;
	void AddSplat(const Point2f &p, const IntegrationResult &v);
	void WriteImage(Float splatScale);

private:
	// Film Private Data
	class Pixel {
	public:
		Pixel() { filterWeightSum = 0; }
		void Initialize(Float binSize, Float maxPathLength) {
			histogram = Histogram(binSize, maxPathLength);
			splatHistogram = Histogram(binSize, maxPathLength);
		}
		
		Histogram histogram;
		Histogram splatHistogram;
		Float filterWeightSum;
	};
	std::unique_ptr<Pixel[]> pixels;
	Float minL;
	Float binSize;
	Float maxPathLength;

	// Film Private Methods
	Pixel &GetPixel(const Point2i &p) {
		Assert(InsideExclusive(p, croppedPixelBounds));
		int width = croppedPixelBounds.pMax.x - croppedPixelBounds.pMin.x;
		int offset = (p.x - croppedPixelBounds.pMin.x) +
			(p.y - croppedPixelBounds.pMin.y) * width;
		return pixels[offset];
	}
};

HistogramFilm *CreateHistogramFilm(const ParamSet &params, std::unique_ptr<Filter> filter);

#endif  // PBRT_FILMS_HISTOGRAMFILM_H
