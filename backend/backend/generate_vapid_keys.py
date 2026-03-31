"""
SmartPlant — VAPID Key Generator
═══════════════════════════════════
Run once to generate VAPID keys for Web Push notifications.
Prints the keys as environment variables to set.

Usage:
    python generate_vapid_keys.py
"""

import base64
import os

from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives import serialization


def generate_vapid_keys():
    """Generate a VAPID (Voluntary Application Server Identification) keypair."""
    # Generate EC private key on P-256 curve
    private_key = ec.generate_private_key(ec.SECP256R1())

    # Export private key as raw bytes (32 bytes)
    private_numbers = private_key.private_numbers()
    private_bytes = private_numbers.private_value.to_bytes(32, byteorder="big")

    # Export public key as uncompressed point (65 bytes: 0x04 + x + y)
    public_key = private_key.public_key()
    public_numbers = public_key.public_numbers()
    x_bytes = public_numbers.x.to_bytes(32, byteorder="big")
    y_bytes = public_numbers.y.to_bytes(32, byteorder="big")
    public_uncompressed = b"\x04" + x_bytes + y_bytes

    # URL-safe base64 encode (no padding)
    private_b64 = base64.urlsafe_b64encode(private_bytes).rstrip(b"=").decode("ascii")
    public_b64 = base64.urlsafe_b64encode(public_uncompressed).rstrip(b"=").decode("ascii")

    return private_b64, public_b64


def main():
    private_key, public_key = generate_vapid_keys()

    print("=" * 60)
    print("  SmartPlant — VAPID Keys Generated")
    print("=" * 60)
    print()
    print("Set these environment variables before running the backend:")
    print()
    print(f"  SMARTPLANT_VAPID_PRIVATE_KEY={private_key}")
    print(f"  SMARTPLANT_VAPID_PUBLIC_KEY={public_key}")
    print()
    print("Public key (for frontend):")
    print(f"  {public_key}")
    print()
    print("On Windows (PowerShell):")
    print(f'  $env:SMARTPLANT_VAPID_PRIVATE_KEY="{private_key}"')
    print(f'  $env:SMARTPLANT_VAPID_PUBLIC_KEY="{public_key}"')
    print()
    print("On Linux/macOS:")
    print(f'  export SMARTPLANT_VAPID_PRIVATE_KEY="{private_key}"')
    print(f'  export SMARTPLANT_VAPID_PUBLIC_KEY="{public_key}"')


if __name__ == "__main__":
    main()
