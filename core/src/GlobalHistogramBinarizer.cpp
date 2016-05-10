/*
* Copyright 2016 ZXing authors
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/

#include "GlobalHistogramBinarizer.h"
#include "LuminanceSource.h"
#include "BitArray.h"
#include "BitMatrix.h"
#include "ByteArray.h"
#include "ErrorStatus.h"

#include <array>

namespace ZXing {

static const int LUMINANCE_BITS = 5;
static const int LUMINANCE_SHIFT = 8 - LUMINANCE_BITS;
static const int LUMINANCE_BUCKETS = 1 << LUMINANCE_BITS;


GlobalHistogramBinarizer::GlobalHistogramBinarizer(const std::shared_ptr<const LuminanceSource>& source, bool pureBarcode) :
	_source(source),
	_pureBarcode(pureBarcode)
{
}

bool
GlobalHistogramBinarizer::isPureBarcode() const
{
	return _pureBarcode;
}

int
GlobalHistogramBinarizer::width() const
{
	return _source->width();
}

int
GlobalHistogramBinarizer::height() const
{
	return _source->height();
}


// Return -1 on error
static int EstimateBlackPoint(const std::array<int, LUMINANCE_BUCKETS>& buckets)
{
	// Find the tallest peak in the histogram.
	int maxBucketCount = 0;
	int firstPeak = 0;
	int firstPeakSize = 0;
	for (int x = 0; x < LUMINANCE_BUCKETS; x++) {
		if (buckets[x] > firstPeakSize) {
			firstPeak = x;
			firstPeakSize = buckets[x];
		}
		if (buckets[x] > maxBucketCount) {
			maxBucketCount = buckets[x];
		}
	}

	// Find the second-tallest peak which is somewhat far from the tallest peak.
	int secondPeak = 0;
	int secondPeakScore = 0;
	for (int x = 0; x < LUMINANCE_BUCKETS; x++) {
		int distanceToBiggest = x - firstPeak;
		// Encourage more distant second peaks by multiplying by square of distance.
		int score = buckets[x] * distanceToBiggest * distanceToBiggest;
		if (score > secondPeakScore) {
			secondPeak = x;
			secondPeakScore = score;
		}
	}

	// Make sure firstPeak corresponds to the black peak.
	if (firstPeak > secondPeak) {
		int temp = firstPeak;
		firstPeak = secondPeak;
		secondPeak = temp;
	}

	// If there is too little contrast in the image to pick a meaningful black point, throw rather
	// than waste time trying to decode the image, and risk false positives.
	if (secondPeak - firstPeak <= LUMINANCE_BUCKETS / 16) {
		return -1;
	}

	// Find a valley between them that is low and closer to the white peak.
	int bestValley = secondPeak - 1;
	int bestValleyScore = -1;
	for (int x = secondPeak - 1; x > firstPeak; x--) {
		int fromFirst = x - firstPeak;
		int score = fromFirst * fromFirst * (secondPeak - x) * (maxBucketCount - buckets[x]);
		if (score > bestValleyScore) {
			bestValley = x;
			bestValleyScore = score;
		}
	}

	return bestValley << LUMINANCE_SHIFT;
}

// Applies simple sharpening to the row data to improve performance of the 1D Readers.
ErrorStatus
GlobalHistogramBinarizer::getBlackRow(int y, BitArray& row) const
{
	int width = _source->width();
	row.init(width);

	ByteArray buffer;
	const uint8_t* luminances = _source->getRow(y, buffer);
	std::array<int, LUMINANCE_BUCKETS> buckets = {};
	for (int x = 0; x < width; x++) {
		int pixel = luminances[x] & 0xff;
		buckets[pixel >> LUMINANCE_SHIFT]++;
	}
	int blackPoint = EstimateBlackPoint(buckets);
	if (blackPoint >= 0) {

		int left = luminances[0];
		int center = luminances[1];
		for (int x = 1; x < width - 1; x++) {
			int right = luminances[x + 1];
			// A simple -1 4 -1 box filter with a weight of 2.
			int luminance = ((center * 4) - left - right) / 2;
			if (luminance < blackPoint) {
				row.set(x);
			}
			left = center;
			center = right;
		}
		return ErrorStatus::NoError;
	}
	return ErrorStatus::NotFound;
}

static void InitBlackMatrix(const LuminanceSource& source, std::shared_ptr<const BitMatrix>& outMatrix)
{
	auto matrix = std::make_shared<BitMatrix>();
	int width = source.width();
	int height = source.height();
	matrix->init(width, height);

	// Quickly calculates the histogram by sampling four rows from the image. This proved to be
	// more robust on the blackbox tests than sampling a diagonal as we used to do.
	std::array<int, LUMINANCE_BUCKETS> localBuckets = {};
	{
		ByteArray buffer;
		for (int y = 1; y < 5; y++) {
			int row = height * y / 5;
			const uint8_t* luminances = source.getRow(row, buffer);
			int right = (width * 4) / 5;
			for (int x = width / 5; x < right; x++) {
				int pixel = luminances[x] & 0xff;
				localBuckets[pixel >> LUMINANCE_SHIFT]++;
			}
		}
	}

	int blackPoint = EstimateBlackPoint(localBuckets);
	if (blackPoint >= 0) {
		// We delay reading the entire image luminance until the black point estimation succeeds.
		// Although we end up reading four rows twice, it is consistent with our motto of
		// "fail quickly" which is necessary for continuous scanning.
		ByteArray buffer;
		int stride;
		const uint8_t* luminances = source.getMatrix(buffer, stride);
		for (int y = 0; y < height; y++) {
			int offset = y * stride;
			for (int x = 0; x < width; x++) {
				int pixel = luminances[offset + x] & 0xff;
				if (pixel < blackPoint) {
					matrix->set(x, y);
				}
			}
		}
		outMatrix = matrix;
	}
}

// Does not sharpen the data, as this call is intended to only be used by 2D Readers.
std::shared_ptr<const BitMatrix>
GlobalHistogramBinarizer::getBlackMatrix() const
{
	std::call_once(_matrixOnce, &InitBlackMatrix, *_source, _matrix);
	return _matrix;
}

bool
GlobalHistogramBinarizer::canCrop() const
{
	return _source->canCrop();
}

std::shared_ptr<BinaryBitmap>
GlobalHistogramBinarizer::cropped(int left, int top, int width, int height) const
{
	return newInstance(_source->cropped(left, top, width, height));
}

bool
GlobalHistogramBinarizer::canRotate() const
{
	return _source->canRotate();
}

std::shared_ptr<BinaryBitmap>
GlobalHistogramBinarizer::rotated(int degreeCW) const
{
	return newInstance(_source->rotated(degreeCW));
}

std::shared_ptr<BinaryBitmap>
GlobalHistogramBinarizer::newInstance(const std::shared_ptr<const LuminanceSource>& source) const
{
	return std::make_shared<GlobalHistogramBinarizer>(source, _pureBarcode);
}


} // ZXing