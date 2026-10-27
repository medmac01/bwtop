// metalbench: repeated Metal blit copies between large private buffers; reports rd+wr GB/s.
import Metal
import Foundation
let dev = MTLCreateSystemDefaultDevice()!
let q = dev.makeCommandQueue()!
let sz = 512 << 20
let a = dev.makeBuffer(length: sz, options: .storageModePrivate)!
let b = dev.makeBuffer(length: sz, options: .storageModePrivate)!
let t0 = Date()
var copies = 0
while Date().timeIntervalSince(t0) < 5 {
    let cb = q.makeCommandBuffer()!
    let e = cb.makeBlitCommandEncoder()!
    for _ in 0..<8 { e.copy(from: a, sourceOffset: 0, to: b, destinationOffset: 0, size: sz); copies += 1 }
    e.endEncoding(); cb.commit(); cb.waitUntilCompleted()
}
let dt = Date().timeIntervalSince(t0)
print(String(format: "metalbench: %.1f GB/s (rd+wr)", Double(copies) * 2 * Double(sz) / dt / 1e9))
