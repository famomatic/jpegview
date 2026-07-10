![CI](https://github.com/famomatic/jpegview/actions/workflows/ci-test.yml/badge.svg?branch=master)
![Build x64](https://github.com/famomatic/jpegview/actions/workflows/build-release-x64.yml/badge.svg?branch=master)
![Build Win32](https://github.com/famomatic/jpegview/actions/workflows/build-release-win32.yml/badge.svg?branch=master)
![Release](https://github.com/famomatic/jpegview/actions/workflows/release.yml/badge.svg)
[![OS Support](https://img.shields.io/badge/Windows-10%20%7C%2011-blue)](#system-requirements)
[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue)](https://github.com/famomatic/jpegview/blob/master/LICENSE.txt)
[![Latest Release](https://img.shields.io/github/v/release/famomatic/jpegview?label=GitHub&style=social)](https://github.com/famomatic/jpegview/releases)

# JPEGView - Image Viewer and Editor

This is a fork of [JPEGView by sylikc](https://github.com/sylikc/jpegview), which itself continues the legacy of the excellent [JPEGView by David Kleiner](https://sourceforge.net/projects/jpegview/). It extends the original with a modern CMake + vcpkg build, a broad set of new image formats, an optional D3D11 GPU processing backend, and a number of workflow features (batch conversion, EXIF editing, on-disk thumbnail cache, and more).

## Description

JPEGView is a lean, fast and highly configurable image viewer/editor with a minimal GUI.

### Formats Supported

JPEGView has built-in support for the following formats:

* Popular: JPEG, GIF, ICO/CUR
* Lossless: BMP, PNG (incl. animated PNG), TIFF (incl. BigTIFF, multi-page, all compression modes), PSD (8/16/32-bit, all compression modes)
* Web: WEBP (incl. animated), JXL (incl. animated), HEIF/HEIC, AVIF (incl. animated)
* Vector: SVG, SVGZ (NanoSVG)
* Game/CG: DDS (BC1/BC3 + uncompressed), EXR (OpenEXR), HDR/PIC (Radiance RGBE)
* Legacy: TGA, WDP, HDP, JXR (libjxr), JPEG 2000 (JP2/J2K/J2C, OpenJPEG), QOI
* Camera RAW formats:
  * Adobe (DNG), Canon (CRW, CR2, CR3), Nikon (NEF, NRW), Sony (ARW, SR2)
  * Olympus (ORF), Panasonic (RW2), Fujifilm (RAF)
  * Sigma (X3F), Pentax (PEF), Minolta (MRW), Kodak (KDC, DCR)
  * A full list is available here: [LibRaw supported cameras](https://www.libraw.org/supported-cameras)

JPEG XR (JXR/WDP/HDP) is decoded natively via libjxr instead of relying on WIC. TIFF is decoded via libtiff (with BigTIFF and lazy partial loading for multi-gigabyte images). Additional formats may still be supported by Windows Imaging Component (WIC) as a fallback.

#### Save Formats

Images can be saved to: JPEG, BMP, PNG, TIFF, WEBP (lossy/lossless), TGA, GIF, QOI, and the modern formats JPEG XL, AVIF, and HEIF/HEIC. Save quality for the modern formats is configurable via the `JXLSaveQuality`, `AVIFSaveQuality`, and `HEIFSaveQuality` INI settings.

### Basic Image Editor

Basic on-the-fly image processing is provided - allowing adjusting typical parameters:

* sharpness, color balance, contrast, local under-exposure/over-exposure (LDC)
* rotation (incl. arbitrary angle) and perspective correction
* unsharp mask, color saturation
* lossless JPEG transformations (rotate, mirror, crop, optimize)

### Other Features

* Small and fast; runtime-dispatched SIMD (SSE2/AVX2 via google/highway) and up to 64 CPU cores
* High quality resampling filter (now 4-channel BGRA, alpha filtered with the same kernel as RGB)
* Optional D3D11 GPU image-processing backend (compute shaders for resampling, unsharp mask, LDC, LUT, Gauss) - off by default, auto-falls back to CPU
* On-disk thumbnail cache so revisiting a file is instant (path + size + mtime keyed, LRU eviction)
* Batch conversion dialog (select files, pick target format, set quality, optionally resize)
* EXIF metadata editing for JPEG (orientation, GPS removal) and "Copy EXIF info" to clipboard
* Processing presets, pixel/color probe (live RGB + HSV readout), and view bookmarks (9 slots)
* Zoom/pan history navigation, smart crop (auto-detect borders), favorite/recent folder quick-open, A/B compare
* Live background switching for transparent images (black / white / checkerboard)
* Ultra-high-res image support via lazy partial loading (multi-gigabyte TIFFs load in tens of MB)
* Movie/Slideshow mode - to play a folder of images as a movie
* Basic image processing tools can be applied realtime during viewing

# Installation

## Official Releases

Releases are published on the [GitHub Releases](https://github.com/famomatic/jpegview/releases) page. Each release includes:

* **Portable ZIP** - 64-bit and 32-bit builds
* **Windows Installer MSI** - 64-bit and 32-bit installers
* **Source code** - Build it yourself

## Portable

JPEGView _does not require installation_ to run. Just **unzip, and run** either the 64-bit version, or the 32-bit version depending on which platform you're on. It can save the settings to the extracted folder and run entirely portable.

## MSI Installer

For those who prefer to have JPEGView installed for All Users, a 32-bit/64-bit installer is available to download starting with v1.0.40.

(The MSI release is not code-signed. Please verify checksums!)

## System Requirements

* 64-bit version: Windows 10 or 11 (64-bit)
* 32-bit version: Windows 10 (32-bit)

Windows 7/8 are no longer actively tested. The build toolchain is Visual Studio 2022 (v143) with C++20, which targets Windows 10+; older OSes may still run but are not guaranteed.

# Building from Source

JPEGView builds with CMake + Visual Studio 2022 (v143) and resolves all image-codec dependencies through vcpkg (manifest mode, static linkage). See [COMPILING.txt](COMPILING.txt) for the full guide.

Quick start:

```bat
git clone --recurse-submodules https://github.com/famomatic/jpegview.git
cd jpegview

:: set VCPKG_ROOT once (points at your vcpkg checkout)
setx VCPKG_ROOT D:\vcpkg

:: configure + build (x64 Release shown)
cmake --preset x64-release
cmake --build --preset x64-release
```

Four presets are available: `x64-debug`, `x64-release`, `x86-debug`, `x86-release`. The compiled `JPEGView.exe`, `WICLoader.dll`, and config files land under `build/<preset>/out/<config>/`. Codec libraries are statically linked, so no separate DLLs are needed at runtime.

To build the WiX MSI installer, enable `JPEGVIEW_BUILD_SETUP=ON` (requires WiX Toolset v3.11+ on PATH):

```bat
cmake --preset x64-release -DJPEGVIEW_BUILD_SETUP=ON
cmake --build --preset x64-release --target JPEGView.Setup
```

## Continuous Integration

CI is driven by GitHub Actions on `windows-2022` runners. The workflows live under [.github/workflows](.github/workflows):

* **CI (build + ctest)** ([ci-test.yml](.github/workflows/ci-test.yml)) - runs on every push/PR to `master`. Configures with CMake + vcpkg, builds the CPU and GPU test targets, and runs `ctest`.
* **Build x64 / Win32 (Release)** ([build-release-x64.yml](.github/workflows/build-release-x64.yml), [build-release-win32.yml](.github/workflows/build-release-win32.yml)) - manual (`workflow_dispatch`) full Release builds that upload artifacts.
* **Build Test x64 / Win32 (Debug)** ([build-debug-x64.yml](.github/workflows/build-debug-x64.yml), [build-debug-win32.yml](.github/workflows/build-debug-win32.yml)) - manual Debug builds.
* **Release** ([release.yml](.github/workflows/release.yml)) - triggered by `v*` tags. Builds both x86 and x64 Release + MSI, then creates a draft GitHub Release with portable ZIPs and MSIs attached.

The reusable build pipeline is in [workflow-build-wtl-submodule.yml](.github/workflows/workflow-build-wtl-submodule.yml), backed by composite actions under [.github/actions](.github/actions) (`setup-wix`, `wtl-submodule`, `build-solution`, `upload-some-artifacts`).

## What's New

* See what has changed in the [latest releases](https://github.com/famomatic/jpegview/releases)
* Or check the [CHANGELOG.txt](CHANGELOG.txt) to review new features in detail.

Recent highlights (v1.4 - v1.6):

* **v1.6** - JXL/AVIF/HEIF save support, batch conversion dialog, EXIF editing, zoom history, smart crop, favorite/recent folders, A/B compare
* **v1.5** - On-disk thumbnail cache, processing presets + pixel probe + view bookmarks, live background switching, D3D11 GPU backend, libtiff TIFF reader with BigTIFF, lazy partial loading for ultra-high-res images, google/highway SIMD resampling
* **v1.4** - SVG/SVGZ, ICO/CUR, DDS, JPEG 2000, HDR/PIC, JPEG XR (libjxr), OpenEXR, full 16/32-bit PSD, animated PNG + ICC coexistence, CMake + vcpkg build migration

# Localization

By default, the language is auto-detected to match your Windows Locale. All the text in the menus and user interface should show in your language. To override the auto-detection, manually set `Language` option in `JPEGView.ini`

JPEGView is currently translated/localized to 28 languages:

| INI Option | Language |
| ---------- | -------- |
| be | Belarusian |
| bg | Bulgarian |
| cs | Czech |
| de | German |
| el | Greek, Modern |
| es-ar | Spanish (Argentina) |
| es | Spanish |
| eu | Basque |
| fi | Finnish |
| fr | French (Français) |
| hu | Hungarian |
| it | Italian |
| ja | Japanese (日本語) |
| ko | Korean (한국어) |
| pl | Polish |
| pt-br | Portuguese (Brazilian) |
| pt | Portuguese |
| ro | Romanian |
| ru | Russian (Русский) |
| sk | Slovak |
| sl | Slovenian (Slovenščina) |
| sr | Serbian (српски) |
| sv | Swedish |
| ta | Tamil |
| tr | Turkish (Türkçe) |
| uk | Ukrainian (Українська) |
| zh-tw | Chinese, Traditional (繁體中文) |
| zh | Chinese, Simplified (简体中文) |

# Help / Documentation

The JPEGView documentation is a little out of date at the moment, but should still give a good summary of the features.

This [readme.html](https://htmlpreview.github.io/?https://github.com/famomatic/jpegview/blob/master/src/JPEGView/Config/readme.html) is part of the JPEGView package.

# Brief History

This repo is a fork of [JPEGView by sylikc](https://github.com/sylikc/jpegview), which continues the legacy (is itself a "fork") of the excellent project [JPEGView by David Kleiner](https://sourceforge.net/projects/jpegview/). Unfortunately, starting in 2020, the SourceForge project has essentially been abandoned, with the last update being [2018-02-24 (1.0.37)](https://sourceforge.net/projects/jpegview/files/jpegview/). It's an excellent lightweight image viewer.

## Special Thanks

Special thanks to [qbnu](https://github.com/qbnu) for adding additional codec support!
* Animated WebP, Animated PNG, JPEG XL (with animation), HEIF/HEIC/AVIF, QOI
* ICC Profile support for WebP, JPEG XL, HEIF/HEIC, AVIF
* LibRaw support (all updated RAW formats, such as CR3)
* Photoshop PSD support

Thanks to [sylikc](https://github.com/sylikc) for maintaining the fork this project builds upon.

Thanks to all the _translators_ which keep JPEGView strings up-to-date in different languages! See [CHANGELOG.txt](CHANGELOG.txt) to find credits for translators at each release!