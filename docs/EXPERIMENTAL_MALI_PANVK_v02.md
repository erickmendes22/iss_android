# Experimental Mali/PanVK — ISS Android v0.2 (without FSR)

Source baseline: 3473136401d845ac8621a21a103efc8be65411fc,
the v0.2 Bionic CI build completed before first FSR3 commit 296d833e.
The public release tag points to the initialization commit on main rather
than the Bionic APK source; the experimental branch deliberately uses the
successful pre-FSR release-era commit.

Bloodborne-Android: PanVK v3.2 universal Mali CSF driver. SHA256 is pinned
in stage_panvk.py; packaging checks ELF64/AArch64, Android Bionic and Vulkan
ICD/HAL exports. It is bundled beside unmodified Turnip Gen8 V38 and v27 R1.

Automatic selection:
- Adreno 8xx: Turnip Gen8 V38.
- Adreno 6xx/7xx: Turnip v27 R1.
- Mali (/dev/mali0, no KGSL), Android 15+: PanVK v3.2 experimental.
- Older Android or other GPUs: system Vulkan.
- Explicit manual PanVK is refused on ineligible devices; manual system
  and existing imported Turnip choices remain available.

No post-v0.2 FSR code is merged. This is driver integration, not proof
that inFAMOUS graphics/gameplay operate on Mali. Real Mali CSF device
testing, Vulkan feature and presentation logs are still necessary.
