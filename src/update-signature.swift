// SPDX-License-Identifier: GPL-3.0-or-later
// Ed25519 verification for update signatures, with Apple's CryptoKit.
// Called from plugin-updater.mm; see OBSAirPlayVerifyUpdateSignature.
import CryptoKit
import Foundation

@_cdecl("obs_airplay_ed25519_verify")
public func obs_airplay_ed25519_verify(_ publicKey: UnsafePointer<UInt8>,
                                       _ message: UnsafePointer<UInt8>, _ messageLength: Int,
                                       _ signature: UnsafePointer<UInt8>) -> Int32
{
    guard let key = try? Curve25519.Signing.PublicKey(
        rawRepresentation: Data(bytes: publicKey, count: 32)) else {
        return 0
    }
    return key.isValidSignature(Data(bytes: signature, count: 64),
                                for: Data(bytes: message, count: messageLength)) ? 1 : 0
}
