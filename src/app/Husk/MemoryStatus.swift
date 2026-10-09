// SPDX-License-Identifier: GPL-2.0-or-later
import Foundation
import Darwin
import MachO
import os

/// What iOS lets Husk use: the two permissions that raise its limits, as the running app was signed, and what they come to.
///
/// Both are entitlements, which iOS honours only when the app's signature carries them (and the signing account allowed it): when
/// Husk runs inside LiveContainer, LiveContainer's signature is the one that counts. GetMoreRam adds the increased memory limit to
/// a sideloaded app's ID. The extended address space is what big games need: iOS gives an app far fewer addresses than Android
/// does, and a game that reserves a lot (Unity 6's heap, the APK it maps) runs out of them long before it runs out of memory.
enum MemoryStatus {
    static let increasedLimitKey = "com.apple.developer.kernel.increased-memory-limit"
    static let extendedAddressingKey = "com.apple.developer.kernel.extended-virtual-addressing"

    /// The main executable's entitlements (LiveContainer's inside LiveContainer), as XML and as DER, read from its code signature.
    private static let signed: (xml: String?, der: Data?) = readEntitlements()

    /// Whether the running app was signed with this entitlement set to true.
    static func isSigned(_ key: String) -> Bool {
        if let xml = signed.xml, let r = xml.range(of: "<key>\(key)</key>") {
            return xml[r.upperBound...].trimmingCharacters(in: .whitespacesAndNewlines).hasPrefix("<true/>")
        }
        if let der = signed.der, let k = key.data(using: .utf8) { return der.range(of: k) != nil }
        return false
    }

    static var increasedMemoryLimit: Bool { isSigned(increasedLimitKey) }
    static var extendedAddressing: Bool { isSigned(extendedAddressingKey) }

    private static func vmInfo() -> task_vm_info_data_t? {
        var info = task_vm_info_data_t()
        var count = mach_msg_type_number_t(MemoryLayout<task_vm_info_data_t>.size / MemoryLayout<natural_t>.size)
        let kr = withUnsafeMutablePointer(to: &info) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) {
                task_info(mach_task_self_, task_flavor_t(TASK_VM_INFO), $0, &count)
            }
        }
        return kr == KERN_SUCCESS ? info : nil
    }

    /// Where iOS would close the app: the memory it uses now and what it may still take.
    static var memoryLimit: UInt64 { (vmInfo()?.phys_footprint ?? 0) + UInt64(os_proc_available_memory()) }

    /// How far the app's addresses reach: the extended address space moves this out by tens of GB.
    static var addressLimit: UInt64 { vmInfo()?.max_address ?? 0 }

    static func gb(_ bytes: UInt64) -> String { String(format: "%.1f GB", Double(bytes) / 1_073_741_824) }

    /// The line for the log: what was signed and what it came to.
    static var summary: String {
        "increased memory limit \(increasedMemoryLimit ? "on" : "off") (iOS closes the app at about \(gb(memoryLimit))), "
            + "extended address space \(extendedAddressing ? "on" : "off") (addresses up to \(gb(addressLimit)))"
    }

    /// The entitlement blobs of the main executable's code signature, read from its __LINKEDIT in memory.
    private static func readEntitlements() -> (xml: String?, der: Data?) {
        guard let header = _dyld_get_image_header(0) else { return (nil, nil) }
        let slide = _dyld_get_image_vmaddr_slide(0)
        var linkedit: (vmaddr: UInt64, fileoff: UInt64)?
        var signature: (off: UInt32, size: UInt32)?
        var p = UnsafeRawPointer(header).advanced(by: MemoryLayout<mach_header_64>.size)
        for _ in 0..<header.pointee.ncmds {
            let lc = p.loadUnaligned(as: load_command.self)
            if lc.cmd == UInt32(LC_SEGMENT_64) {
                let seg = p.loadUnaligned(as: segment_command_64.self)
                let name = withUnsafeBytes(of: seg.segname) { String(decoding: $0.prefix { $0 != 0 }, as: UTF8.self) }
                if name == "__LINKEDIT" { linkedit = (seg.vmaddr, seg.fileoff) }
            } else if lc.cmd == UInt32(LC_CODE_SIGNATURE) {
                let cs = p.loadUnaligned(as: linkedit_data_command.self)
                signature = (cs.dataoff, cs.datasize)
            }
            p = p.advanced(by: Int(lc.cmdsize))
        }
        guard let le = linkedit, let sig = signature, sig.size >= 12,
              let base = UnsafeRawPointer(bitPattern: Int(le.vmaddr) + slide + Int(sig.off) - Int(le.fileoff)) else { return (nil, nil) }
        let size = Int(sig.size)
        func be32(_ off: Int) -> UInt32? {
            off >= 0 && off + 4 <= size ? UInt32(bigEndian: base.loadUnaligned(fromByteOffset: off, as: UInt32.self)) : nil
        }
        guard be32(0) == 0xfade0cc0, let count = be32(8) else { return (nil, nil) }
        var xml: String?, der: Data?
        for i in 0..<Int(min(count, 64)) {
            guard let o = be32(16 + i * 8) else { break }
            let off = Int(o)
            guard let magic = be32(off), let len = be32(off + 4), len >= 8, off + Int(len) <= size else { continue }
            let body = UnsafeRawBufferPointer(start: base.advanced(by: off + 8), count: Int(len) - 8)
            if magic == 0xfade7171 { xml = String(decoding: body, as: UTF8.self) }
            if magic == 0xfade7172 { der = Data(body) }
        }
        return (xml, der)
    }
}
