// SPDX-License-Identifier: GPL-3.0-or-later
//
// Generates the synthetic format corpus (SPEC 12) with Apple ImageIO.
// Usage: gen_corpus <output-dir>
//
// Every still image uses the same "quadrant" layout in *display* orientation so
// tests can verify orientation and color handling by sampling the corners:
//   top-left = red, top-right = green, bottom-left = blue, bottom-right = white
// (variants with alpha make the bottom-right quadrant transparent).
// The expected values live in tests/corpus/expected.json – keep both in sync.
// Formats the running macOS cannot encode are skipped (logged as "skip").

import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

guard CommandLine.arguments.count >= 2 else {
    FileHandle.standardError.write("usage: gen_corpus <output-dir>\n".data(using: .utf8)!)
    exit(2)
}
let outDir = URL(fileURLWithPath: CommandLine.arguments[1], isDirectory: true)
try? FileManager.default.createDirectory(at: outDir, withIntermediateDirectories: true)

typealias RGBA = (r: Double, g: Double, b: Double, a: Double)

let sRGB = CGColorSpace(name: CGColorSpace.sRGB)!
let displayP3 = CGColorSpace(name: CGColorSpace.displayP3)!
let linearSRGBExtended = CGColorSpace(name: CGColorSpace.extendedLinearSRGB)!
let linearP3Extended = CGColorSpace(name: CGColorSpace.extendedLinearDisplayP3)!
let genericCMYK = CGColorSpace(name: CGColorSpace.genericCMYK)!
let grayGamma22 = CGColorSpace(name: CGColorSpace.genericGrayGamma2_2)!

var written: [String] = []
var skipped: [String] = []

// MARK: - pixel helpers

/// Quadrant pattern in display coordinates.
func quad(_ x: Int, _ y: Int, _ w: Int, _ h: Int, alpha: Bool = false) -> RGBA {
    let left = x < w / 2, top = y < h / 2
    switch (top, left) {
    case (true, true): return (1, 0, 0, 1)
    case (true, false): return (0, 1, 0, 1)
    case (false, true): return (0, 0, 1, 1)
    default: return alpha ? (0, 0, 0, 0) : (1, 1, 1, 1)
    }
}

/// Builds an RGB(A) CGImage. bpc 8/16 = integer, 32 = float.
func makeImage(_ w: Int, _ h: Int, space: CGColorSpace = sRGB, bpc: Int = 8, alpha: Bool = false,
               _ pixel: (Int, Int) -> RGBA) -> CGImage {
    let bytesPerComp = bpc / 8
    let bpr = w * 4 * bytesPerComp
    let alphaInfo = alpha ? CGImageAlphaInfo.premultipliedLast : CGImageAlphaInfo.noneSkipLast
    var info = alphaInfo.rawValue
    switch bpc {
    case 8: info |= CGBitmapInfo.byteOrder32Big.rawValue
    case 16: info |= CGBitmapInfo.byteOrder16Little.rawValue
    default: info |= CGBitmapInfo.floatComponents.rawValue | CGBitmapInfo.byteOrder32Little.rawValue
    }
    let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: bpc, bytesPerRow: bpr,
                        space: space, bitmapInfo: info)!
    let base = ctx.data!
    for y in 0..<h {
        for x in 0..<w {
            var p = pixel(x, y)
            if !alpha { p.a = 1 }
            let c = [p.r * p.a, p.g * p.a, p.b * p.a, p.a]
            let off = y * bpr + x * 4 * bytesPerComp
            for i in 0..<4 {
                switch bpc {
                case 8:
                    base.storeBytes(of: UInt8(max(0, min(255, (c[i] * 255).rounded()))),
                                    toByteOffset: off + i, as: UInt8.self)
                case 16:
                    base.storeBytes(of: UInt16(max(0, min(65535, (c[i] * 65535).rounded()))).littleEndian,
                                    toByteOffset: off + i * 2, as: UInt16.self)
                default:
                    base.storeBytes(of: Float(c[i]), toByteOffset: off + i * 4, as: Float.self)
                }
            }
        }
    }
    return ctx.makeImage()!
}

func makeCMYK(_ w: Int, _ h: Int) -> CGImage {
    let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                        space: genericCMYK, bitmapInfo: CGImageAlphaInfo.none.rawValue)!
    let base = ctx.data!.assumingMemoryBound(to: UInt8.self)
    for y in 0..<h {
        for x in 0..<w {
            let left = x < w / 2, top = y < h / 2
            // TL cyan, TR magenta, BL yellow, BR black (pure K)
            let cmyk: [UInt8] = top ? (left ? [255, 0, 0, 0] : [0, 255, 0, 0])
                                    : (left ? [0, 0, 255, 0] : [0, 0, 0, 255])
            for i in 0..<4 { base[y * w * 4 + x * 4 + i] = cmyk[i] }
        }
    }
    return ctx.makeImage()!
}

func makeGray(_ w: Int, _ h: Int) -> CGImage {
    let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w,
                        space: grayGamma22, bitmapInfo: CGImageAlphaInfo.none.rawValue)!
    let base = ctx.data!.assumingMemoryBound(to: UInt8.self)
    for y in 0..<h {
        for x in 0..<w {
            let left = x < w / 2, top = y < h / 2
            base[y * w + x] = top ? (left ? 0 : 85) : (left ? 170 : 255)
        }
    }
    return ctx.makeImage()!
}

/// Stores the display-oriented quadrant pattern so that applying EXIF `orientation`
/// yields the canonical pattern. Returns the stored (unoriented) image.
func orientedQuad(_ dW: Int, _ dH: Int, orientation: Int) -> CGImage {
    let swap = orientation >= 5
    let sW = swap ? dH : dW, sH = swap ? dW : dH
    var stored = [RGBA](repeating: (0, 0, 0, 1), count: sW * sH)
    for y in 0..<dH {
        for x in 0..<dW {
            let (c, r): (Int, Int)
            switch orientation {
            case 2: (c, r) = (dW - 1 - x, y)
            case 3: (c, r) = (dW - 1 - x, dH - 1 - y)
            case 4: (c, r) = (x, dH - 1 - y)
            case 5: (c, r) = (y, x)
            case 6: (c, r) = (y, dW - 1 - x)
            case 7: (c, r) = (dH - 1 - y, dW - 1 - x)
            case 8: (c, r) = (dH - 1 - y, x)
            default: (c, r) = (x, y)
            }
            stored[r * sW + c] = quad(x, y, dW, dH)
        }
    }
    return makeImage(sW, sH) { x, y in stored[y * sW + x] }
}

// MARK: - writing

@discardableResult
func write(_ name: String, _ type: UTType, _ frames: [(CGImage, [CFString: Any])],
           fileProps: [CFString: Any] = [:], expectCount: Int? = nil) -> Bool {
    let url = outDir.appendingPathComponent(name)
    try? FileManager.default.removeItem(at: url)
    guard let dest = CGImageDestinationCreateWithURL(url as CFURL, type.identifier as CFString, frames.count, nil)
    else {
        skipped.append("\(name) (no encoder for \(type.identifier))")
        return false
    }
    if !fileProps.isEmpty { CGImageDestinationSetProperties(dest, fileProps as CFDictionary) }
    for (img, props) in frames { CGImageDestinationAddImage(dest, img, props as CFDictionary) }
    var ok = CGImageDestinationFinalize(dest)
    // Read back: the file must decode and contain the expected number of images.
    if ok, let src = CGImageSourceCreateWithURL(url as CFURL, nil) {
        let count = CGImageSourceGetCount(src)
        if let want = expectCount, count != want {
            skipped.append("\(name) (encoder wrote \(count) images, expected \(want))")
            ok = false
        } else if CGImageSourceCreateImageAtIndex(src, 0, nil) == nil {
            skipped.append("\(name) (written file does not decode)")
            ok = false
        }
    } else if ok {
        skipped.append("\(name) (written file unreadable)")
        ok = false
    } else {
        skipped.append("\(name) (finalize failed)")
    }
    if ok { written.append(name) } else { try? FileManager.default.removeItem(at: url) }
    return ok
}

func type(_ id: String) -> UTType { UTType(id) ?? UTType(importedAs: id) }

func writeBytes(_ name: String, _ data: Data) {
    let url = outDir.appendingPathComponent(name)
    try? data.write(to: url)
    written.append(name)
}

/// Removes color chunks (iCCP, sRGB, gAMA, cHRM, cICP) from a PNG -> "untagged".
func stripPngColorChunks(_ data: Data) -> Data {
    var out = Data(data.prefix(8))
    var pos = 8
    let bytes = [UInt8](data)
    while pos + 8 <= bytes.count {
        let len = Int(bytes[pos]) << 24 | Int(bytes[pos + 1]) << 16 | Int(bytes[pos + 2]) << 8 | Int(bytes[pos + 3])
        let type = String(bytes: bytes[(pos + 4)..<(pos + 8)], encoding: .ascii) ?? ""
        let total = 12 + len
        if !["iCCP", "sRGB", "gAMA", "cHRM", "cICP"].contains(type) {
            out.append(contentsOf: bytes[pos..<min(bytes.count, pos + total)])
        }
        pos += total
    }
    return out
}

let W = 64, H = 48

// MARK: - JPEG

for o in 1...8 {
    let img = orientedQuad(W, H, orientation: o)
    write("orient_\(o).jpg", .jpeg, [(img, [kCGImagePropertyOrientation: o,
                                            kCGImageDestinationLossyCompressionQuality: 0.95])])
}
write("progressive.jpg", .jpeg,
      [(makeImage(W, H) { quad($0, $1, W, H) },
        [kCGImagePropertyJFIFDictionary: [kCGImagePropertyJFIFIsProgressive: true],
         kCGImageDestinationLossyCompressionQuality: 0.95])])
write("cmyk.jpg", .jpeg, [(makeCMYK(W, H), [kCGImageDestinationLossyCompressionQuality: 0.95])])

// JPEG with EXIF/GPS metadata + embedded thumbnail; XMP rating added in a 2nd pass.
do {
    let base = makeImage(640, 480) { quad($0, $1, 640, 480) }
    let props: [CFString: Any] = [
        kCGImageDestinationEmbedThumbnail: true,
        kCGImageDestinationLossyCompressionQuality: 0.9,
        kCGImagePropertyTIFFDictionary: [
            kCGImagePropertyTIFFMake: "Lumaflip",
            kCGImagePropertyTIFFModel: "Synthetic Camera X1",
        ],
        kCGImagePropertyExifDictionary: [
            kCGImagePropertyExifFNumber: 2.8,
            kCGImagePropertyExifExposureTime: 0.004,
            kCGImagePropertyExifISOSpeedRatings: [400],
            kCGImagePropertyExifFocalLength: 35.0,
            kCGImagePropertyExifLensModel: "Synthetic 35mm F1.4",
            kCGImagePropertyExifDateTimeOriginal: "2024:05:17 14:30:05",
            kCGImagePropertyExifOffsetTimeOriginal: "+02:00",
            kCGImagePropertyExifExposureBiasValue: -0.7,
            kCGImagePropertyExifFlash: 16,
        ],
        kCGImagePropertyGPSDictionary: [
            kCGImagePropertyGPSLatitude: 48.1371,
            kCGImagePropertyGPSLatitudeRef: "N",
            kCGImagePropertyGPSLongitude: 122.4194,
            kCGImagePropertyGPSLongitudeRef: "W",
            kCGImagePropertyGPSAltitude: 519.0,
            kCGImagePropertyGPSAltitudeRef: 0,
        ],
    ]
    let tmp = outDir.appendingPathComponent(".exif_tmp.jpg")
    if let dest = CGImageDestinationCreateWithURL(tmp as CFURL, UTType.jpeg.identifier as CFString, 1, nil) {
        CGImageDestinationAddImage(dest, base, props as CFDictionary)
        CGImageDestinationFinalize(dest)
        let meta = CGImageMetadataCreateMutable()
        CGImageMetadataSetValueWithPath(meta, nil, "xmp:Rating" as CFString, 4 as CFNumber)
        let url = outDir.appendingPathComponent("exif.jpg")
        if let src = CGImageSourceCreateWithURL(tmp as CFURL, nil),
           let dest2 = CGImageDestinationCreateWithURL(url as CFURL, UTType.jpeg.identifier as CFString, 1, nil)
        {
            let opts: [CFString: Any] = [kCGImageDestinationMetadata: meta, kCGImageDestinationMergeMetadata: true]
            if CGImageDestinationCopyImageSource(dest2, src, opts as CFDictionary, nil) {
                written.append("exif.jpg")
            } else {
                skipped.append("exif.jpg (metadata copy failed)")
            }
        }
        try? FileManager.default.removeItem(at: tmp)
    }
}

// 24 MP JPEG for the P-01/P-04 benchmarks (with embedded EXIF thumbnail).
do {
    let bw = 6000, bh = 4000
    let ctx = CGContext(data: nil, width: bw, height: bh, bitsPerComponent: 8, bytesPerRow: 0, space: sRGB,
                        bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue | CGBitmapInfo.byteOrder32Big.rawValue)!
    let grad = CGGradient(colorsSpace: sRGB,
                          colors: [CGColor(red: 0.1, green: 0.3, blue: 0.6, alpha: 1),
                                   CGColor(red: 0.9, green: 0.7, blue: 0.3, alpha: 1)] as CFArray,
                          locations: [0, 1])!
    ctx.drawLinearGradient(grad, start: .zero, end: CGPoint(x: bw, y: bh), options: [])
    var seed: UInt64 = 0x1234_5678
    func rnd() -> Double {
        seed = seed &* 6364136223846793005 &+ 1442695040888963407
        return Double(seed >> 11) / Double(1 << 53)
    }
    for _ in 0..<4000 {
        ctx.setFillColor(CGColor(red: rnd(), green: rnd(), blue: rnd(), alpha: 0.6))
        let r = CGRect(x: rnd() * Double(bw), y: rnd() * Double(bh), width: 20 + rnd() * 400, height: 20 + rnd() * 400)
        if rnd() < 0.5 { ctx.fillEllipse(in: r) } else { ctx.fill(r) }
    }
    write("big_24mp.jpg", .jpeg, [(ctx.makeImage()!, [kCGImageDestinationLossyCompressionQuality: 0.85,
                                                      kCGImageDestinationEmbedThumbnail: true])])
}

// MARK: - PNG

write("rgba8.png", .png, [(makeImage(W, H, alpha: true) { quad($0, $1, W, H, alpha: true) }, [:])])
write("rgba16.png", .png, [(makeImage(W, H, bpc: 16, alpha: true) { quad($0, $1, W, H, alpha: true) }, [:])])
write("srgb.png", .png, [(makeImage(W, H) { quad($0, $1, W, H) }, [:])])
// Display P3 tagged: TL = P3(0.8, 0.3, 0.2) which is not representable 1:1 after flattening to sRGB.
write("p3.png", .png, [(makeImage(W, H, space: displayP3) { x, y in
    let q = quad(x, y, W, H)
    return (q.r == 1 && q.g == 0 && q.b == 0) ? (0.8, 0.3, 0.2, 1) : q
}, [:])])
write("gray.png", .png, [(makeGray(W, H), [:])])
if let data = try? Data(contentsOf: outDir.appendingPathComponent("srgb.png")) {
    writeBytes("untagged.png", stripPngColorChunks(data))
    writeBytes("png_as_jpg.jpg", data)  // F-FMT-01: signature wins over the suffix
}

// MARK: - animations

let frameColors: [RGBA] = [(1, 0, 0, 1), (0, 1, 0, 1), (0, 0, 1, 1)]
let delays = [0.1, 0.2, 0.3]
func frames(_ dictKey: CFString, _ delayKey: CFString) -> [(CGImage, [CFString: Any])] {
    (0..<3).map { i in
        (makeImage(W, H) { _, _ in frameColors[i] }, [dictKey: [delayKey: delays[i]]])
    }
}
// Single frames as PNG for external encoders (animated WebP via img2webp).
do {
    let fdir = outDir.appendingPathComponent(".frames")
    try? FileManager.default.createDirectory(at: fdir, withIntermediateDirectories: true)
    for (i, f) in frames(kCGImagePropertyPNGDictionary, kCGImagePropertyAPNGDelayTime).enumerated() {
        let url = fdir.appendingPathComponent("f\(i).png")
        if let d = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil) {
            CGImageDestinationAddImage(d, f.0, nil)
            CGImageDestinationFinalize(d)
        }
    }
}
write("anim.gif", .gif, frames(kCGImagePropertyGIFDictionary, kCGImagePropertyGIFDelayTime),
      fileProps: [kCGImagePropertyGIFDictionary: [kCGImagePropertyGIFLoopCount: 0]], expectCount: 3)
write("anim.png", .png, frames(kCGImagePropertyPNGDictionary, kCGImagePropertyAPNGDelayTime),
      fileProps: [kCGImagePropertyPNGDictionary: [kCGImagePropertyAPNGLoopCount: 0]], expectCount: 3)
write("anim.heics", type("public.heics"), frames(kCGImagePropertyHEICSDictionary, kCGImagePropertyHEICSDelayTime),
      fileProps: [kCGImagePropertyHEICSDictionary: [kCGImagePropertyHEICSLoopCount: 0]], expectCount: 3)

// MARK: - modern formats

write("still.heic", .heic, [(makeImage(W, H) { quad($0, $1, W, H) }, [kCGImageDestinationLossyCompressionQuality: 0.95])])
write("alpha.heic", .heic, [(makeImage(W, H, alpha: true) { quad($0, $1, W, H, alpha: true) },
                             [kCGImageDestinationLossyCompressionQuality: 0.95])])
write("still.avif", type("public.avif"), [(makeImage(W, H) { quad($0, $1, W, H) },
                                          [kCGImageDestinationLossyCompressionQuality: 0.95])])

// MARK: - TIFF

write("multipage.tiff", .tiff, [
    (makeImage(W, H) { quad($0, $1, W, H) }, [:]),
    (makeImage(32, 24) { quad($0, $1, 32, 24) }, [:]),
    (makeImage(16, 12) { quad($0, $1, 16, 12) }, [:]),
], expectCount: 3)
write("rgb16.tiff", .tiff, [(makeImage(W, H, bpc: 16) { quad($0, $1, W, H) }, [:])])
write("cmyk.tiff", .tiff, [(makeCMYK(W, H), [:])])

// MARK: - legacy / misc

write("quad.bmp", .bmp, [(makeImage(W, H) { quad($0, $1, W, H) }, [:])])
write("icon.ico", .ico, [
    (makeImage(32, 32, alpha: true) { quad($0, $1, 32, 32) }, [:]),
    (makeImage(16, 16, alpha: true) { quad($0, $1, 16, 16) }, [:]),
], expectCount: 2)
write("icon.icns", .icns, [(makeImage(32, 32, alpha: true) { quad($0, $1, 32, 32) }, [:])])
write("quad.tga", type("com.truevision.tga-image"), [(makeImage(W, H) { quad($0, $1, W, H) }, [:])])
write("quad.psd", type("com.adobe.photoshop-image"), [(makeImage(W, H) { quad($0, $1, W, H) }, [:])])
write("quad.jp2", type("public.jpeg-2000"), [(makeImage(W, H) { quad($0, $1, W, H) },
                                              [kCGImageDestinationLossyCompressionQuality: 1.0])])
write("quad16.psd", type("com.adobe.photoshop-image"), [(makeImage(W, H, bpc: 16) { quad($0, $1, W, H) }, [:])])

// MARK: - GPU texture containers, Radiance
// ImageIO does not recognise these from the first bytes alone (ImageIODecoder::canDecode
// checks their magic numbers itself).
// DDS: uncompressed 32-bit BGRA (ImageIO's own DDS encoder writes files it cannot read back).
do {
    var dds = Data("DDS ".utf8)
    func u32(_ v: UInt32) { withUnsafeBytes(of: v.littleEndian) { dds.append(contentsOf: $0) } }
    u32(124)                                   // dwSize
    u32(0x1 | 0x2 | 0x4 | 0x8 | 0x1000)        // CAPS | HEIGHT | WIDTH | PITCH | PIXELFORMAT
    u32(UInt32(H)); u32(UInt32(W)); u32(UInt32(W * 4)); u32(0); u32(0)
    for _ in 0..<11 { u32(0) }                 // dwReserved1
    u32(32); u32(0x40 | 0x1)                   // DDS_PIXELFORMAT: size, RGB | ALPHAPIXELS
    u32(0); u32(32)                            // FourCC, bit count
    u32(0x00FF_0000); u32(0x0000_FF00); u32(0x0000_00FF); u32(0xFF00_0000)
    u32(0x1000); u32(0); u32(0); u32(0); u32(0) // caps (TEXTURE), caps2-4, reserved2
    for y in 0..<H {
        for x in 0..<W {
            let q = quad(x, y, W, H)
            dds.append(contentsOf: [UInt8(q.b * 255), UInt8(q.g * 255), UInt8(q.r * 255), 255])
        }
    }
    writeBytes("quad.dds", dds)
}
// KTX from float pixels: ImageIO reads integer data back from its own KTX files as half floats.
write("quad.ktx", type("org.khronos.ktx"), [(makeImage(W, H, space: linearSRGBExtended, bpc: 32) { quad($0, $1, W, H) }, [:])])
write("quad.ktx2", type("org.khronos.ktx2"), [(makeImage(W, H) { quad($0, $1, W, H) }, [:])])
// Radiance RGBE, flat (uncompressed) scanlines; ImageIO reads but does not write it.
do {
    var hdr = Data("#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y \(H) +X \(W)\n".utf8)
    for y in 0..<H {
        for x in 0..<W {
            let q = quad(x, y, W, H)
            // 1.0 = mantissa 128, exponent 129 (2^1 * 128/256)
            hdr.append(contentsOf: [q.r > 0 ? 128 : 0, q.g > 0 ? 128 : 0, q.b > 0 ? 128 : 0, 129])
        }
    }
    writeBytes("quad.hdr", hdr)
}

// MARK: - HDR

// OpenEXR, linear float; the red quadrant is 4x SDR white (HDR highlight).
write("hdr.exr", type("com.ilm.openexr-image"), [(makeImage(W, H, space: linearSRGBExtended, bpc: 32) { x, y in
    let q = quad(x, y, W, H)
    return (q.r == 1 && q.g == 0) ? (4, 0, 0, 1) : q
}, [:])])
// Gain-map HEIC from an extended-range image (ISO 21496-1 gain map, macOS 15+).
do {
    let hdrBase = makeImage(W, H, space: linearP3Extended, bpc: 32) { x, y in
        let q = quad(x, y, W, H)
        return (q.r == 1 && q.g == 1) ? (4, 4, 4, 1) : q
    }
    // Without a known content headroom the encoder writes a degenerate gain map.
    let hdr = CGImageCreateCopyWithContentHeadroom(4.0, hdrBase) ?? hdrBase
    write("gainmap.heic", .heic, [(hdr, [kCGImageDestinationEncodeRequest: kCGImageDestinationEncodeToISOGainmap,
                                         kCGImageDestinationLossyCompressionQuality: 0.95])])
    // The same as JPEG: primary image + MPF + gain map JPEG behind it (Ultra HDR layout).
    write("gainmap.jpg", .jpeg, [(hdr, [kCGImageDestinationEncodeRequest: kCGImageDestinationEncodeToISOGainmap,
                                        kCGImageDestinationLossyCompressionQuality: 0.95])])
    // PQ (Rec. 2100) 16-bit AVIF.
    let pqSpace = CGColorSpace(name: CGColorSpace.itur_2100_PQ)!
    let pq = makeImage(W, H, space: pqSpace, bpc: 16) { x, y in
        let q = quad(x, y, W, H)
        return (q.r == 1 && q.g == 1) ? (0.75, 0.75, 0.75, 1) : (q.r * 0.5, q.g * 0.5, q.b * 0.5, 1)
    }
    write("pq.avif", type("public.avif"), [(pq, [kCGImageDestinationLossyCompressionQuality: 0.95])])
}

// MARK: - broken files (F-FMT-02)

if let data = try? Data(contentsOf: outDir.appendingPathComponent("big_24mp.jpg")) {
    writeBytes("truncated.jpg", data.prefix(data.count / 3))
}
writeBytes("empty.jpg", Data())
do {
    var seed: UInt32 = 42
    var bytes = [UInt8]()
    for _ in 0..<4096 {
        seed = seed &* 1103515245 &+ 12345
        bytes.append(UInt8(truncatingIfNeeded: seed >> 16))
    }
    bytes[0] = 0x42  // make sure no known signature
    bytes[1] = 0x00
    writeBytes("garbage.jpg", Data(bytes))
}

// MARK: - "Rest" formats (own decoders, SPEC 4.4 M3): hand-encoded, quadrant layout

/// 8-bit RGBA quadrant pixels (alpha 255).
func quadBytes(_ w: Int, _ h: Int) -> [[UInt8]] {
    var px: [[UInt8]] = []
    for y in 0..<h {
        for x in 0..<w {
            let q = quad(x, y, w, h)
            px.append([UInt8(q.r * 255), UInt8(q.g * 255), UInt8(q.b * 255), 255])
        }
    }
    return px
}
func be32(_ v: Int) -> [UInt8] { [UInt8((v >> 24) & 255), UInt8((v >> 16) & 255), UInt8((v >> 8) & 255), UInt8(v & 255)] }
func be16(_ v: Int) -> [UInt8] { [UInt8((v >> 8) & 255), UInt8(v & 255)] }
func le16(_ v: Int) -> [UInt8] { [UInt8(v & 255), UInt8((v >> 8) & 255)] }

do { // QOI: RGBA ops and runs
    let px = quadBytes(W, H)
    var out: [UInt8] = Array("qoif".utf8) + be32(W) + be32(H) + [4, 0]
    var prev: [UInt8] = [0, 0, 0, 255]
    var run = 0
    for p in px {
        if p == prev {
            run += 1
            if run == 62 { out.append(0xC0 | UInt8(run - 1)); run = 0 }
            continue
        }
        if run > 0 { out.append(0xC0 | UInt8(run - 1)); run = 0 }
        out += [0xFF] + p
        prev = p
    }
    if run > 0 { out.append(0xC0 | UInt8(run - 1)) }
    out += [0, 0, 0, 0, 0, 0, 0, 1]
    writeBytes("quad.qoi", Data(out))
}
do { // PCX: 8 bit, 3 planes, RLE
    var hdr = [UInt8](repeating: 0, count: 128)
    hdr[0] = 0x0A; hdr[1] = 5; hdr[2] = 1; hdr[3] = 8
    hdr.replaceSubrange(8..<12, with: le16(W - 1) + le16(H - 1))
    hdr[65] = 3
    hdr.replaceSubrange(66..<68, with: le16(W))
    hdr[68] = 1
    var out = hdr
    let px = quadBytes(W, H)
    for y in 0..<H {
        for c in 0..<3 {
            var x = 0
            while x < W {
                let v = px[y * W + x][c]
                var n = 1
                while x + n < W && n < 63 && px[y * W + x + n][c] == v { n += 1 }
                if n > 1 || v >= 0xC0 { out += [0xC0 | UInt8(n), v] } else { out.append(v) }
                x += n
            }
        }
    }
    writeBytes("quad.pcx", Data(out))
}
do { // PPM (8 bit), PPM (16 bit), PFM (float, white quadrant at 4.0 = HDR)
    let px = quadBytes(W, H)
    var p6: [UInt8] = Array("P6\n# Lumaflip corpus\n\(W) \(H)\n255\n".utf8)
    var p16: [UInt8] = Array("P6\n\(W) \(H)\n65535\n".utf8)
    for p in px {
        p6 += p[0..<3]
        for c in 0..<3 { p16 += be16(Int(p[c]) * 257) }
    }
    writeBytes("quad.ppm", Data(p6))
    writeBytes("quad16.ppm", Data(p16))
    var pfm: [UInt8] = Array("PF\n\(W) \(H)\n-1.0\n".utf8)
    for y in (0..<H).reversed() { // bottom-up
        for x in 0..<W {
            let p = px[y * W + x]
            let white = p[0] == 255 && p[1] == 255 && p[2] == 255
            for c in 0..<3 {
                let f: Float = white ? 4.0 : Float(p[c]) / 255
                withUnsafeBytes(of: f.bitPattern.littleEndian) { pfm += $0 }
            }
        }
    }
    writeBytes("hdr.pfm", Data(pfm))
}
do { // SGI: RLE, 8 bit, RGBA (literal packets)
    let px = quadBytes(W, H)
    let z = 4
    var header = [UInt8](repeating: 0, count: 512)
    header.replaceSubrange(0..<12, with: be16(474) + [1, 1] + be16(3) + be16(W) + be16(H) + be16(z))
    header.replaceSubrange(16..<20, with: be32(255))
    var rows: [[UInt8]] = []
    for c in 0..<z {
        for y in 0..<H { // bottom-up
            var row: [UInt8] = []
            var x = 0
            while x < W {
                let n = min(127, W - x)
                row.append(0x80 | UInt8(n))
                for k in 0..<n { row.append(px[(H - 1 - y) * W + x + k][c]) }
                x += n
            }
            row.append(0)
            rows.append(row)
        }
    }
    var offset = 512 + rows.count * 8
    var starts: [UInt8] = [], lengths: [UInt8] = []
    for r in rows { starts += be32(offset); lengths += be32(r.count); offset += r.count }
    writeBytes("quad.sgi", Data(header + starts + lengths + rows.flatMap { $0 }))
}

// MARK: - SVG (text)

let svg = """
<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="200" height="100" viewBox="0 0 200 100">
  <rect x="0" y="0" width="100" height="50" fill="#ff0000"/>
  <rect x="100" y="0" width="100" height="50" fill="#00ff00"/>
  <rect x="0" y="50" width="100" height="50" fill="#0000ff"/>
  <rect x="100" y="50" width="100" height="50" fill="#ffffff"/>
</svg>

"""
writeBytes("vector.svg", svg.data(using: .utf8)!)

print("written: \(written.count) files")
for s in skipped { print("skip: \(s)") }
