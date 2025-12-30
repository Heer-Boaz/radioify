type BufferRegion = { start: number; end: number; colorTag: string; label: string };

const LEFT_BLOCKS = ['▏', '▎', '▍', '▌', '▋', '▊', '▉', '█'];
const SLIVER_THRESHOLD = 1 / 16;
const quantizeCoverage = (value: number) => Math.min(8, Math.max(0, Math.round(value * 8 + 1e-7)));
const GAP_FG_TAG = '{black-fg}';

const HEX_TABLE = Array.from({ length: 256 }, (_, i) => i.toString(16).padStart(2, '0'));
const FG_CACHE = new Map<number, string>();
const MAX_FG_CACHE_SIZE = 4096;

const SRGB_TO_LINEAR = (() => {
	const table = new Float32Array(256);
	for (let i = 0; i < 256; i++) {
		const s = i / 255;
		table[i] = s <= 0.04045 ? s / 12.92 : ((s + 0.055) / 1.055) ** 2.4;
	}
	return table;
})();

/**
 * Renders a buffer bar with fractional rendering at the boundaries and full blocks in the interior.
 * Overlapping regions are shown by layering a foreground sliver over a background region per cell.
 *
 * @param unfilteredRegions - Array of regions to render, each with a start, end, color tag, and label.
 * @param totalSize - The total size of the buffer being represented.
 * @param barLength - The length of the bar in characters.
 * @returns A string representing the rendered buffer bar with color tags and labels.
 */
export function renderBufferBar(
	unfilteredRegions: Array<{ start: number; end: number; colorTag: string, label: string }>,
	totalSize: number,
	barLength: number
): string {
	const quantize = quantizeCoverage;
	const cellSize = totalSize / barLength;
	const defaultCellChar = '·';
	const cellChars = new Array(barLength).fill(defaultCellChar);
	const cellColors = new Array(barLength).fill('');

	// Filter out empty regions (start === end === 0)
	let regions = unfilteredRegions.filter(region => region.start !== 0 || region.end !== 0);
	// Concatenate all neighbouring regions with the same colorTag
	// Note that this is actually not strictly necessary for the rendering,
	// but it simplifies the rendering logic and avoids unnecessary complexity.
	{
		const mergedRegions: BufferRegion[] = [];
		for (const region of regions) {
			const last = mergedRegions[mergedRegions.length - 1];
			if (last && last.colorTag === region.colorTag && region.start <= last.end) {
				last.end = Math.max(last.end, region.end);
			} else {
				mergedRegions.push({ start: region.start, end: region.end, colorTag: region.colorTag, label: region.label });
			}
		}
		regions = mergedRegions;
	}

	const toBackground = (colorTag: string) => {
		// Convert color tag to background color by replacing -fg with -bg
		return colorTag.replace('-fg}', '-bg}');
	};
	const glyphForCoverage = (coverage: number) => {
		const idx = quantize(coverage);
		return idx === 0 ? '' : LEFT_BLOCKS[idx - 1];
	};
	for (let cell = 0; cell < barLength; cell++) {
		const cellStart = cell * cellSize;
		const cellEnd = cellStart + cellSize;

		let fgRegion: BufferRegion = null;
		let bgRegion: BufferRegion = null;
		let fgStart = 0;
		let fgEnd = 0;

		for (const region of regions) {
			const segStart = Math.max(region.start, cellStart);
			const segEnd = Math.min(region.end, cellEnd);
			const overlap = segEnd - segStart;
			if (overlap <= 0) continue;
			if (!fgRegion) {
				if (overlap / cellSize < SLIVER_THRESHOLD) continue;
				fgRegion = region;
				fgStart = segStart;
				fgEnd = segEnd;
				continue;
			}
			bgRegion = region;
			break;
		}

		if (!fgRegion) continue;

		const leftFrac = (fgStart - cellStart) / cellSize;
		const rightFrac = (fgEnd - cellStart) / cellSize;
		const coverage = rightFrac - leftFrac;

		if (coverage <= 0) continue;

		if (coverage >= 1 - 1e-7) {
			cellChars[cell] = '█';
			cellColors[cell] = fgRegion.colorTag;
			continue;
		}

		const leftGap = leftFrac;
		const rightGap = 1 - rightFrac;
		const alignRight = leftGap > 0 && (rightGap <= 0 || rightGap < leftGap);

		if (alignRight) {
			if (leftGap < SLIVER_THRESHOLD) {
				cellChars[cell] = '█';
				cellColors[cell] = fgRegion.colorTag;
				continue;
			}
			const glyph = glyphForCoverage(leftGap);
			if (glyph) {
				cellChars[cell] = glyph;
				const gapFg = bgRegion ? bgRegion.colorTag : GAP_FG_TAG;
				cellColors[cell] = toBackground(fgRegion.colorTag) + gapFg;
			}
		} else {
			if (coverage < SLIVER_THRESHOLD) continue;
			const glyph = glyphForCoverage(coverage);
			if (glyph) {
				cellChars[cell] = glyph;
				cellColors[cell] = (bgRegion ? toBackground(bgRegion.colorTag) : '') + fgRegion.colorTag;
			}
		}
	}

	const bar = cellChars.map((ch, i) => cellColors[i] + ch + '{/}').join('');

	// --- Generate legend from summaryRegions ---
	// Collect unique colorTag/label pairs, preserving order of first appearance
	const legendMap = new Map<string, string>();
	for (const region of regions) {
		if (region.colorTag && region.label && !legendMap.has(region.label)) {
			legendMap.set(region.label, region.colorTag);
		}
	}
	// Sort legend as per the order of appearance in the regions array
	const sortedLegend = Array.from(legendMap.keys()).sort((a, b) => {
		const aIndex = regions.findIndex(r => r.label === a);
		const bIndex = regions.findIndex(r => r.label === b);
		return aIndex - bIndex;
	});

	// Compose legend string
	const legend = sortedLegend
		.map(label => `${legendMap.get(label)}█{/${legendMap.get(label).replace('{', '').replace('-fg}', '')}-fg} ${label}`)
		.join('  ');


	return `[${bar}]\n${legend}`;
}

/**
 * Renders a simple summary bar using the same overlap layering as renderBufferBar.
 */
export function renderSummaryBar(
	regions: Array<{ start: number, end: number, colorTag: string, label: string }>,
	totalSize: number,
	barLength: number,
): string {
	return renderBufferBar(regions, totalSize, barLength);
}

/**
 * Generates pixel-perfect ASCII art from an image buffer.
 * Each pixel is represented by a colored block character, ensuring high fidelity to the original image.
 * Transparent pixels are rendered as spaces, while opaque pixels are rendered with their respective colors.
 *
 * @param imgBuf - The image buffer (RGBA format, Buffer or Uint8Array).
 * @param imgW - The width of the image in pixels.
 * @param imgH - The height of the image in pixels.
 * @returns The generated ASCII art string using colored block characters.
 */
export function generatePixelPerfectAsciiArt(
	imgBuf: Buffer | Uint8Array,
	imgW: number,
	imgH: number,
): string {
	const lines: string[] = [];
	for (let y = 0; y < imgH; y++) {
		const segments: string[] = [];
		let runTag = '{/}';
		let runLen = 0;
		const flush = () => {
			if (!runLen) return;
			if (runTag === '{/}') {
				segments.push(runTag + ' '.repeat(runLen));
			} else {
				segments.push(runTag + '█'.repeat(runLen) + '{/}');
			}
			runLen = 0;
		};
		for (let x = 0; x < imgW; x++) {
			const idx4 = (y * imgW + x) * 4;
			const r = imgBuf[idx4];
			const g = imgBuf[idx4 + 1];
			const b = imgBuf[idx4 + 2];
			const a = imgBuf[idx4 + 3];
			const tag = a < 64 ? '{/}' : fgTagFromRGB(r, g, b);
			if (tag === runTag) {
				++runLen;
			} else {
				flush();
				runTag = tag;
				runLen = 1;
			}
		}
		flush();
		lines.push(segments.join(''));
	}
	return lines.join('\n') + '\n';
}

/**
 * Generates a braille-based ASCII art representation of an image buffer.
 * Each braille character represents a 2x4 pixel block, allowing for high-density rendering.
 * The function supports optional edge detection and dithering for improved visual quality.
 *
 * @param imgBuf - The image buffer (RGBA, Buffer or Uint8Array).
 * @param imgW - The width of the image in pixels.
 * @param imgH - The height of the image in pixels.
 * @param maxArtWidth - The maximum width of the output ASCII art in characters.
 * @param opts - Optional rendering options:
 *   - useEdgeDetection: Whether to apply edge detection (default: true).
 *   - useDithering: Whether to apply dithering (default: false).
 *   - strictBgDist: Background color distance threshold (default: 32²).
 *   - deltaLum: Luminance difference threshold for dot placement (default: 30).
 * @returns The generated ASCII art string using braille characters.
 */
export function generateBrailleAsciiArt(
	imgBuf: Buffer | Uint8Array,
	imgW: number,
	imgH: number,
	maxArtWidth: number,
	opts: {
		useEdgeDetection?: boolean;   // default true
		useDithering?: boolean;       // default false
		strictBgDist?: number;        // sq-dist voor BG (default 32²)
		deltaLum?: number;            // |Ydiff| drempel (default 30)
	} = {}
): string {

	// --- Scaling logic ---
	// Each braille char is 2x4 pixels, so max output width in chars is (imgW / 2)
	const maxOutW = maxArtWidth - 8;
	let scale = 1;
	if (Math.floor(imgW / 2) > maxOutW) {
		scale = maxOutW / Math.floor(imgW / 2);
	}

	let scaledBuf = imgBuf;
	let scaledW = imgW;
	let scaledH = imgH;

	if (scale < 1) {
		// Downscale image using nearest-neighbor
		scaledW = Math.max(2, Math.floor(imgW * scale));
		scaledH = Math.max(4, Math.floor(imgH * scale));
		scaledBuf = downscaleImageNN(imgBuf, imgW, imgH, scaledW, scaledH);
	}

	const useEdge = opts.useEdgeDetection ?? true;
	const useDith = opts.useDithering ?? false;
	const BG_DIST = opts.strictBgDist ?? 32 * 32;
	const DELTA = opts.deltaLum ?? 30; // luminantie 0-255

	const BRAILLE_BASE = 0x2800;
	const brailleMap = [[0, 1, 2, 6], [3, 4, 5, 7]];
	const outW = Math.min(maxArtWidth - 8, Math.floor(scaledW / 2));
	const outH = Math.max(Math.ceil(scaledH / 4), Math.floor(outW * (scaledH / scaledW) / 2)) + 1;

	/* ---------- gamma-correcte luminantie-buffer ---------- */
	const linY = new Float32Array(scaledW * scaledH);
	{
		let p = 0;
		for (let y = 0; y < scaledH; ++y) {
			for (let x = 0; x < scaledW; ++x, ++p) {
				const i4 = p * 4;
				const r = scaledBuf[i4];
				const g = scaledBuf[i4 + 1];
				const b = scaledBuf[i4 + 2];
				linY[p] = 255 * (0.2126 * SRGB_TO_LINEAR[r] + 0.7152 * SRGB_TO_LINEAR[g] + 0.0722 * SRGB_TO_LINEAR[b]);
			}
		}
	}

	/* ---------- global dominant kleur ---------- */
	const hist = new Map<number, number>();
	for (let p = 0; p < scaledW * scaledH; ++p) {
		const i4 = p << 2;
		if (!scaledBuf[i4 + 3]) continue;                       // transparant
		const key = rgbToKey(scaledBuf[i4], scaledBuf[i4 + 1], scaledBuf[i4 + 2]);
		hist.set(key, (hist.get(key) ?? 0) + 1);
	}
	let bgKey = 0, bgCnt = 0;
	// @ts-ignore
	for (const [k, c] of hist) if (c > bgCnt) { bgCnt = c; bgKey = k; }
	const bgR = bgKey >>> 16 & 255, bgG = bgKey >>> 8 & 255, bgB = bgKey & 255;
	const bgLum = 255 * (0.2126 * srgb2lin(bgR) + 0.7152 * srgb2lin(bgG) + 0.0722 * srgb2lin(bgB));

	/* ---------- dither buffer ---------- */
	const err = useDith ? new Float32Array(scaledW * scaledH) : null;

	/* ---------- edge strength cache ---------- */
	const edges = useEdge ? new Float32Array(scaledW * scaledH) : null;
	if (edges) {
		for (let y = 0; y < scaledH; ++y) {
			for (let x = 0; x < scaledW; ++x) {
				edges[y * scaledW + x] = sobelAt(linY, scaledW, scaledH, x, y);
			}
		}
	}

	/* ---------- render loop ---------- */
	let asciiArt = '';

	for (let cy = 0; cy < outH; ++cy) {
		let line = '';
		for (let cx = 0; cx < outW; ++cx) {

			let k1 = 0, c1 = 0;
			let k2 = 0, c2 = 0;
			let k3 = 0, c3 = 0;
			const vote = (key: number) => {
				if (key === k1) { ++c1; return; }
				if (key === k2) { ++c2; return; }
				if (key === k3) { ++c3; return; }
				if (c1 <= c2 && c1 <= c3) { k1 = key; c1 = 1; return; }
				if (c2 <= c1 && c2 <= c3) { k2 = key; c2 = 1; return; }
				k3 = key; c3 = 1;
			};
			let cellBgR = 0, cellBgG = 0, cellBgB = 0, cellBgCnt = 0;
			let bitmask = 0;

			for (let dy = 0; dy < 4; ++dy) {
				for (let dx = 0; dx < 2; ++dx) {
					const px = Math.min(scaledW - 1, cx * 2 + dx);
					const py = Math.min(scaledH - 1, cy * 4 + dy);
					const p = py * scaledW + px;
					const idx4 = (p * 4);
					const r = scaledBuf[idx4], g = scaledBuf[idx4 + 1], b = scaledBuf[idx4 + 2];

					let yLin = linY[p];
					const nearBg = colorDistSq(r, g, b, bgR, bgG, bgB) < BG_DIST;
					const ditherThisPixel = useDith && !nearBg;   // BG nooit diffusen
					if (ditherThisPixel && err) yLin = clamp(yLin + err[p], 0, 255);

					/* edge-aware Δ-drempel (trekt Δ iets naar beneden op randen) */
				let deltaThr = DELTA;
				if (useEdge && edges) deltaThr = Math.max(10, DELTA - 0.2 * edges[p]);

					const lumDiff = Math.abs(yLin - bgLum);
					const dotSet = !nearBg && lumDiff >= deltaThr;

					if (dotSet) {
						bitmask |= 1 << brailleMap[dx][dy];
						vote(rgbToKey(r, g, b));
					}

					if (nearBg) { cellBgR += r; cellBgG += g; cellBgB += b; ++cellBgCnt; }

					if (ditherThisPixel && err) {
						const target = dotSet ? 0 : 255;
						distributeError(err, yLin - target, p, scaledW, scaledH);
					}
				}
			}

			/* dominante FG-kleur o.b.v. gezette dots */
			let domKey = 0x808080;
			let domCnt = 0;
			if (c1 > domCnt) { domCnt = c1; domKey = k1; }
			if (c2 > domCnt) { domCnt = c2; domKey = k2; }
			if (c3 > domCnt) { domCnt = c3; domKey = k3; }

			const fgTag = `{${keyToHex(domKey)}-fg}`;
			const bgTag = cellBgCnt
				? `{#${hex(cellBgR / cellBgCnt)}${hex(cellBgG / cellBgCnt)}${hex(cellBgB / cellBgCnt)}-bg}`
				: '';

			line += bgTag + fgTag + String.fromCharCode(BRAILLE_BASE + bitmask) + '{/}';
		}
		asciiArt += line + '\n';
	}
	return asciiArt;
}

/**
 * Downscales an image using nearest-neighbor interpolation.
 * This method reduces the dimensions of the image while preserving pixel colors.
 *
 * @param src - The source image buffer (RGBA format, Buffer or Uint8Array).
 * @param srcW - The width of the source image in pixels.
 * @param srcH - The height of the source image in pixels.
 * @param dstW - The desired width of the downscaled image in pixels.
 * @param dstH - The desired height of the downscaled image in pixels.
 * @returns A new Uint8Array containing the downscaled image in RGBA format.
 */
function downscaleImageNN(
	src: Buffer | Uint8Array,
	srcW: number,
	srcH: number,
	dstW: number,
	dstH: number
): Uint8Array {
	const dst = new Uint8Array(dstW * dstH * 4);
	for (let y = 0; y < dstH; ++y) {
		const sy = Math.floor(y * srcH / dstH);
		for (let x = 0; x < dstW; ++x) {
			const sx = Math.floor(x * srcW / dstW);
			const srcIdx = (sy * srcW + sx) * 4;
			const dstIdx = (y * dstW + x) * 4;
			dst[dstIdx] = src[srcIdx];
			dst[dstIdx + 1] = src[srcIdx + 1];
			dst[dstIdx + 2] = src[srcIdx + 2];
			dst[dstIdx + 3] = src[srcIdx + 3];
		}
	}
	return dst;
}

/**
 * Converts an sRGB color component to linear color space.
 * This function applies gamma correction to transform the sRGB value
 * into a linear representation suitable for calculations.
 *
 * @param v - The sRGB color component (0-255).
 * @returns The linear color space value (0-1).
 */
function srgb2lin(v: number) {
	return SRGB_TO_LINEAR[v];
}

/**
 * Converts a numeric value to a two-digit hexadecimal string.
 * The value is clamped between 0 and 255 before conversion.
 *
 * @param v - The numeric value to convert (expected range: 0-255).
 * @returns A two-digit hexadecimal string representing the value.
 */
function hex(v: number) { return HEX_TABLE[Math.round(clamp(v, 0, 255))]; }

/**
 * Clamps a value between a lower and upper bound.
 * If the value is less than the lower bound, the lower bound is returned.
 * If the value is greater than the upper bound, the upper bound is returned.
 * Otherwise, the value itself is returned.
 *
 * @param x - The value to clamp.
 * @param l - The lower bound.
 * @param h - The upper bound.
 * @returns The clamped value.
 */
function clamp(x: number, l: number, h: number) { return x < l ? l : x > h ? h : x; }

/**
 * Converts RGB color components into a single integer key.
 * This key can be used for efficient color comparisons or as a map key.
 *
 * @param r - Red component of the color (0-255).
 * @param g - Green component of the color (0-255).
 * @param b - Blue component of the color (0-255).
 * @returns A 24-bit integer representing the combined RGB color.
 */
function rgbToKey(r: number, g: number, b: number) { return (r << 16) | (g << 8) | b; }

function fgTagFromRGB(r: number, g: number, b: number) {
	const key = rgbToKey(r, g, b);
	let cached = FG_CACHE.get(key);
	if (!cached) {
		cached = `{#${HEX_TABLE[r]}${HEX_TABLE[g]}${HEX_TABLE[b]}-fg}`;
		if (FG_CACHE.size > MAX_FG_CACHE_SIZE) FG_CACHE.clear();
		FG_CACHE.set(key, cached);
	}
	return cached;
}

/**
 * Converts a 24-bit integer color key into a hexadecimal color string.
 * The resulting string is in the format `#RRGGBB`, where `RR`, `GG`, and `BB`
 * are the hexadecimal representations of the red, green, and blue components.
 *
 * @param k - The 24-bit integer color key, where:
 *   - Bits 16-23 represent the red component.
 *   - Bits 8-15 represent the green component.
 *   - Bits 0-7 represent the blue component.
 * @returns A string representing the color in hexadecimal format.
 */
function keyToHex(k: number) { return `#${HEX_TABLE[(k >>> 16) & 0xff]}${HEX_TABLE[(k >>> 8) & 0xff]}${HEX_TABLE[k & 0xff]}`; }

/**
 * Calculates the squared Euclidean distance between two RGB colors.
 * This function is useful for comparing color similarity or detecting
 * differences between colors in a computationally efficient manner.
 *
 * @param r1 - Red component of the first color (0-255).
 * @param g1 - Green component of the first color (0-255).
 * @param b1 - Blue component of the first color (0-255).
 * @param r2 - Red component of the second color (0-255).
 * @param g2 - Green component of the second color (0-255).
 * @param b2 - Blue component of the second color (0-255).
 * @returns The squared distance between the two colors.
 */
function colorDistSq(r1: number, g1: number, b1: number, r2: number, g2: number, b2: number) {
	const dr = r1 - r2, dg = g1 - g2, db = b1 - b2;
	return dr * dr + dg * dg + db * db;
}

/**
 * Computes the Sobel gradient magnitude at a specific pixel in a grayscale buffer.
 * The Sobel operator is used for edge detection by calculating the gradient in both
 * horizontal and vertical directions.
 *
 * @param buf - The grayscale buffer as a Float32Array, where each value represents luminance.
 * @param w - The width of the image in pixels.
 * @param h - The height of the image in pixels.
 * @param x - The x-coordinate of the pixel.
 * @param y - The y-coordinate of the pixel.
 * @returns The gradient magnitude at the specified pixel.
 */
function sobelAt(buf: Float32Array, w: number, h: number, x: number, y: number): number {
	const xm1 = Math.max(0, x - 1), xp1 = Math.min(w - 1, x + 1);
	const ym1 = Math.max(0, y - 1), yp1 = Math.min(h - 1, y + 1);
	const row = y * w;
	const gx = buf[ym1 * w + xp1] + 2 * buf[row + xp1] + buf[yp1 * w + xp1]
		- buf[ym1 * w + xm1] - 2 * buf[row + xm1] - buf[yp1 * w + xm1];
	const gy = buf[yp1 * w + xm1] + 2 * buf[yp1 * w + x] + buf[yp1 * w + xp1]
		- buf[ym1 * w + xm1] - 2 * buf[ym1 * w + x] - buf[ym1 * w + xp1];
	return Math.hypot(gx, gy);
}

/**
 * Distributes the quantization error to neighboring pixels in a dithering process.
 * This function implements Floyd-Steinberg dithering, ensuring smoother transitions
 * between pixel values by propagating the error to adjacent pixels.
 *
 * @param buf - The buffer containing the error values for each pixel.
 * @param e - The quantization error to distribute.
 * @param idx - The index of the current pixel in the buffer.
 * @param w - The width of the image in pixels.
 * @param h - The height of the image in pixels.
 */
function distributeError(buf: Float32Array, e: number, idx: number, w: number, h: number) {
	const x = idx % w, y = Math.floor(idx / w);
	if (x + 1 < w) buf[idx + 1] += e * 7 / 16;
	if (x > 0 && y + 1 < h) buf[idx + w - 1] += e * 3 / 16;
	if (y + 1 < h) buf[idx + w] += e * 5 / 16;
	if (x + 1 < w && y + 1 < h) buf[idx + w + 1] += e * 1 / 16;
}

/**
 * Represents metadata extracted from a RIFF-WAVE audio file.
 * Provides details about the audio format, including bit depth, number of channels,
 * sample rate, and the location of the audio data within the file.
 */
interface WavInfo {
	bits: 8 | 16 | 24 | 32;
	channels: 1 | 2 | 3 | 4;
	sampleRate: number;
	dataOff: number;
	dataLen: number;
}

/**
 * Parses the RIFF-WAVE header and extracts metadata about the audio file.
 * This function supports PCM audio format and provides details such as bit depth,
 * number of channels, sample rate, and the offset and length of the audio data.
 *
 * @param buf - The ArrayBuffer containing the RIFF-WAVE file data.
 * @returns An object containing the parsed WAV metadata:
 *   - bits: Bit depth of the audio (8, 16, 24, or 32 bits).
 *   - channels: Number of audio channels (1, 2, 3, or 4).
 *   - sampleRate: Sample rate of the audio in Hz.
 *   - dataOff: Byte offset of the audio data within the buffer.
 *   - dataLen: Length of the audio data in bytes.
 * @throws Error if the file is not a valid RIFF-WAVE or if the PCM format is unsupported.
 */
export function parseWav(buf: ArrayBuffer): WavInfo {
	const dv = new DataView(buf);

	if (dv.getUint32(0, false) !== 0x52494646) throw new Error('No RIFF');
	if (dv.getUint32(8, false) !== 0x57415645) throw new Error('No WAVE');

	let ptr = 12, fmt: WavInfo = null, dataOff = 0, dataLen = 0;

	while (ptr + 8 <= buf.byteLength) {
		const id = dv.getUint32(ptr, false);
		const size = dv.getUint32(ptr + 4, true);
		if (id === 0x666d7420) {                    // "fmt "
			const audioFmt = dv.getUint16(ptr + 8, true);
			if (audioFmt !== 1) throw new Error('Only PCM supported');
			fmt = {
				channels: dv.getUint16(ptr + 10, true) as 1 | 2 | 3 | 4,
				sampleRate: dv.getUint32(ptr + 12, true),
				bits: dv.getUint16(ptr + 22, true) as 8 | 16 | 24 | 32,
				dataOff: 0,
				dataLen: 0,
			};
		} else if (id === 0x64617461) {             // "data"
			dataOff = ptr + 8;
			dataLen = size;
		}
		ptr += 8 + size + (size & 1);               // pad-byte
	}
	if (!fmt || !dataLen) throw new Error('Invalid WAV: missing fmt or data');
	return { ...fmt, dataOff, dataLen };
}

/**
 * Generates a braille-based ASCII art representation of a PCM audio waveform.
 * Each braille character represents a 2x4 pixel block, allowing for high-density rendering.
 * The function supports multi-channel audio and auto-zoom for better visualization.
 *
 * @param pcm - The PCM audio data as a Uint8Array.
 * @param bits - The bit depth of the audio (8, 16, 24, or 32 bits).
 * @param cols - The number of columns in the output ASCII art.
 * @param baseRows - The base number of rows in the output (default: 80).
 * @param channels - The number of audio channels (default: 1).
 * @param autoZoomFloor - The auto-zoom floor as a fraction of the maximum amplitude (default: 0.25).
 * @returns A string containing the braille-based ASCII art representation of the waveform.
 */
export function asciiWaveBraille(
	pcm: Uint8Array,
	bits: 8 | 16 | 24 | 32,
	cols: number,
	baseRows = 80,
	channels = 1,
	autoZoomFloor = .25           // 0-1   (0.25 ≅ –12 dBFS)
): string {

	const BRAILLE = 0x2800;
	const DOT = [[0, 1, 2, 6], [3, 4, 5, 7]];         // (dx,dy)→bit

	/* ---------- sample → float helper ---------- */
	const BPS = bits >> 3;
	const toF = (i: number): number => {
		if (bits === 8) return (pcm[i] - 128) / 128;
		if (bits === 16) return ((pcm[i] | pcm[i + 1] << 8) << 16 >> 16) / 32768;
		if (bits === 24) return ((pcm[i] | pcm[i + 1] << 8 | pcm[i + 2] << 16) << 8 >> 8) / 8388608;
		return (pcm[i] | pcm[i + 1] << 8 | pcm[i + 2] << 16 | pcm[i + 3] << 24) / 2147483648;
	};

	/* ---------- 1. peaks met zwevende cursor + oversampling ---------- */
	const S = pcm.length / BPS / channels;      // total #samples
	const step = S / (cols * 2);                      // fractie-stap
	const over = Math.ceil(step);               // oversample-margin
	const peaks: [number, number][] = [];

	let pos = 0;
	for (let c = 0; c < cols * 2; ++c) {
		const s0 = pos | 0;
		pos += step;
		const s1 = Math.min(S, (pos | 0) + over); // extra “hold” samples

		let mn = 1, mx = -1;
		for (let s = s0; s < s1; ++s) {
			let v = 0;
			for (let ch = 0; ch < channels; ++ch)
				v += toF((s * channels + ch) * BPS);
			v /= channels;
			if (v < mn) mn = v;
			if (v > mx) mx = v;
		}
		peaks.push([mn, mx]);
	}

	/* ---------- 2. globale max + auto-zoom ---------- */
	let gMax = 0;
	for (const [mn, mx] of peaks)
		gMax = Math.max(gMax, Math.abs(mn), Math.abs(mx));

	// Auto-zoom factor: if the global max is below the auto-zoom floor,
	// we scale the output to ensure visibility of the lowest peaks.
	// The autoZoomFloor is a fraction of the maximum value, e.g., 0.25 means
	// that we want to ensure that the lowest peaks are at least 25% of the maximum.
	const zoom = gMax > 0 && gMax < autoZoomFloor ? autoZoomFloor / gMax : 1;

	// Compute rows based on zoom and baseRows
	// The computation ensures that the number of rows is at least 1
	// and scales the number of rows based on the zoom factor.
	const rows = Math.max(1, Math.floor(baseRows / zoom)); // min 1 rij
	const scale = ((rows * 4 - 1) / 2) * zoom / (gMax || 1);

	/* ---------- 3. braille-grid ---------- */
	const cellCols = Math.max(1, Math.ceil(cols));
	const grid: number[][] = Array.from({ length: rows }, () => Array(cellCols).fill(0));
	const cellPeaks: Array<[number, number]> = Array.from({ length: cellCols }, () => [1, -1] as [number, number]);

	for (let x = 0; x < cols * 2; ++x) {
		const [mn, mx] = peaks[x];
		const cellX = x >> 1;
		if (mn < cellPeaks[cellX][0]) cellPeaks[cellX][0] = mn;
		if (mx > cellPeaks[cellX][1]) cellPeaks[cellX][1] = mx;
		const yMin = Math.round(rows * 4 / 2 - mx * scale);
		const yMax = Math.round(rows * 4 / 2 - mn * scale);

		for (let y = Math.max(0, yMin); y <= Math.min(rows * 4 - 1, yMax); ++y) {
			const cellY = y >> 2;
			const subY = y & 3;
			const subX = x & 1;
			grid[cellY][cellX] |= 1 << DOT[subX][subY];
		}
	}

	/* ---------- 4. naar string ---------- */
	const art = grid
		.map((row, _rowIdx) => row
			.map((code, colIdx) => {
				if (!code) return ' ';
				// Color logic: red for negative, green for positive, yellow for near zero
				const [mn, mx] = cellPeaks[colIdx];
				let colorTag = '';
				if (mn < -0.2 || mx > 0.2) colorTag = '{light-red-fg}';
				else if (mn < -0.1 || mx > 0.1) colorTag = '{light-yellow-fg}';
				else if (mn < 0.1 && mx > -0.1) colorTag = '{light-blue-fg}';
				return colorTag + String.fromCharCode(BRAILLE + code) + '{/}';
			})
			.join(''))
		.join('\n');
	// Remove trailing empty lines
	return art.replace(/(?:[^\S\r\n]*\n)+$/, '').trimEnd();
}
