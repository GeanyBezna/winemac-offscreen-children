// pixstat — count the glchild_overlay colours in a window capture.
// Usage: pixstat IMAGE.png
// Prints, for each colour, the pixel count and the first/last row where it appears, so a
// QA run can tell whether the GDI children are visible (red/blue/yellow) and whether the
// GL frame is upright (green rows above magenta rows).
import Foundation
import CoreGraphics
import ImageIO

let colours: [(String, Int, Int, Int)] = [
    ("red", 220, 30, 30), ("blue", 30, 60, 220), ("yellow", 230, 200, 20),
    ("green", 0, 200, 0), ("magenta", 200, 0, 200),
]
let tolerance = 45

guard CommandLine.arguments.count == 2,
      let source = CGImageSourceCreateWithURL(URL(fileURLWithPath: CommandLine.arguments[1]) as CFURL, nil),
      let image = CGImageSourceCreateImageAtIndex(source, 0, nil) else {
    FileHandle.standardError.write("usage: pixstat IMAGE.png\n".data(using: .utf8)!)
    exit(2)
}
let width = image.width, height = image.height
var pixels = [UInt8](repeating: 0, count: width * height * 4)
let context = CGContext(data: &pixels, width: width, height: height, bitsPerComponent: 8, bytesPerRow: width * 4,
                        space: CGColorSpace(name: CGColorSpace.sRGB)!,
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))

var count = [Int](repeating: 0, count: colours.count)
var first = [Int](repeating: -1, count: colours.count)
var last = [Int](repeating: -1, count: colours.count)
for y in 0..<height {
    for x in 0..<width {
        let o = (y * width + x) * 4
        let r = Int(pixels[o]), g = Int(pixels[o + 1]), b = Int(pixels[o + 2])
        for (i, c) in colours.enumerated() where abs(r - c.1) <= tolerance && abs(g - c.2) <= tolerance && abs(b - c.3) <= tolerance {
            count[i] += 1
            if first[i] < 0 { first[i] = y }
            last[i] = y
        }
    }
}
print("size \(width)x\(height)")
for (i, c) in colours.enumerated() {
    print("\(c.0) \(count[i]) rows \(first[i])-\(last[i])")
}
let g = colours.firstIndex { $0.0 == "green" }!, m = colours.firstIndex { $0.0 == "magenta" }!
if count[g] > 0 && count[m] > 0 {
    print(last[g] < first[m] ? "orientation UPRIGHT" : (last[m] < first[g] ? "orientation FLIPPED" : "orientation MIXED"))
} else {
    print("orientation NONE")
}
