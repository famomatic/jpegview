[![Build x64](https://github.com/famomatic/jpegview/actions/workflows/build-release-x64.yml/badge.svg?branch=master)](https://github.com/famomatic/jpegview/actions/workflows/build-release-x64.yml) [![OS Support](https://img.shields.io/badge/Windows-7%20%7C%208%20%7C%2010%20%7C%2011-blue)](#) [![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue)](https://github.com/famomatic/jpegview/blob/master/LICENSE.txt)

[![Latest GitHub Release](https://img.shields.io/github/v/release/famomatic/jpegview?label=GitHub&style=social)](https://github.com/famomatic/jpegview/releases)

# JPEGView - Image Viewer and Editor

This is a fork of [JPEGView by sylikc](https://github.com/sylikc/jpegview), which itself continues the legacy of the excellent [JPEGView by David Kleiner](https://sourceforge.net/projects/jpegview/).

## Description

JPEGView is a lean, fast and highly configurable image viewer/editor with a minimal GUI.

### Formats Supported

JPEGView has built-in support the following formats:

* Popular: JPEG, GIF, ICO/CUR
* Lossless: BMP, PNG, TIFF, PSD (8/16/32-bit, all compression modes)
* Web: WEBP, JXL, HEIF/HEIC, AVIF
* Vector: SVG, SVGZ (NanoSVG)
* Game/CG: DDS (BC1/BC3/BC7 + uncompressed), EXR (OpenEXR), HDR/PIC (Radiance RGBE)
* Legacy: TGA, WDP, HDP, JXR (libjxr), JPEG 2000 (JP2/J2K/J2C, OpenJPEG)
* Camera RAW formats:
  * Adobe (DNG), Canon (CRW, CR2, CR3), Nikon (NEF, NRW), Sony (ARW, SR2)
  * Olympus (ORF), Panasonic (RW2), Fujifilm (RAF)
  * Sigma (X3F), Pentax (PEF), Minolta (MRW), Kodak (KDC, DCR)
  * A full list is available here: [LibRaw supported cameras](https://www.libraw.org/supported-cameras)

JPEG XR (JXR/WDP/HDP) is decoded natively via libjxr instead of relying on WIC.
Additional formats may still be supported by Windows Imaging Component (WIC) as a fallback.

### Basic Image Editor

Basic on-the-fly image processing is provided - allowing adjusting typical parameters:

* sharpness
* color balance
* rotation
* perspective
* contrast
* local under-exposure/over-exposure

### Other Features

* Small and fast, uses AVX2/SSE2 and up to 4 CPU cores
* High quality resampling filter, preserving sharpness of images
* Basic image processing tools can be applied realtime during viewing
* Movie/Slideshow mode - to play folder of JPEGs as movie

# Installation

## Official Releases

Releases are published on the [GitHub Releases](https://github.com/famomatic/jpegview/releases) page.  Each release includes:

* **Archive Zip/7z** - Portable
* **Windows Installer MSI** - For Installs
* **Source code** - Build it yourself

## Portable

JPEGView _does not require installation_ to run.  Just **unzip, and run** either the 64-bit version, or the 32-bit version depending on which platform you're on.  It can save the settings to the extracted folder and run entirely portable.

## MSI Installer

For those who prefer to have JPEGView installed for All Users, a 32-bit/64-bit installer is available to download starting with v1.0.40.

(The MSI release is not code-signed.  Please verify checksums!)

## System Requirements

* 64-bit version: Windows 7/8/10/11 64-bit or later
* 32-bit version: Windows 7 or later

## What's New

* See what has changed in the [latest releases](https://github.com/famomatic/jpegview/releases)
* Or check the [CHANGELOG.txt](https://github.com/famomatic/jpegview/blob/master/CHANGELOG.txt) to review new features in detail.

# Localization

By default, the language is auto-detected to match your Windows Locale.  All the text in the menus and user interface should show in your language.  To override the auto-detection, manually set `Language` option in `JPEGView.ini`

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

The JPEGView documentation is a little out of the date at the moment, but should still give a good summary of the features.

This [readme.html](https://htmlpreview.github.io/?https://github.com/famomatic/jpegview/blob/master/src/JPEGView/Config/readme.html) is part of the JPEGView package.

# Brief History

This repo is a fork of [JPEGView by sylikc](https://github.com/sylikc/jpegview), which continues the legacy (is itself a "fork") of the excellent project [JPEGView by David Kleiner](https://sourceforge.net/projects/jpegview/).  Unfortunately, starting in 2020, the SourceForge project has essentially been abandoned, with the last update being [2018-02-24 (1.0.37)](https://sourceforge.net/projects/jpegview/files/jpegview/).  It's an excellent lightweight image viewer.

## Special Thanks

Special thanks to [qbnu](https://github.com/qbnu) for adding additional codec support!
* Animated WebP
* Animated PNG
* JPEG XL with animation support
* HEIF/HEIC/AVIF support
* QOI support
* ICC Profile support for WebP, JPEG XL, HEIF/HEIC, AVIF
* LibRaw support (all updated RAW formats, such as CR3)
* Photoshop PSD support

Thanks to [sylikc](https://github.com/sylikc) for maintaining the fork this project builds upon.

Thanks to all the _translators_ which keep JPEGView strings up-to-date in different languages!  See [CHANGELOG.txt](https://github.com/famomatic/jpegview/blob/master/CHANGELOG.txt) to find credits for translators at each release!
