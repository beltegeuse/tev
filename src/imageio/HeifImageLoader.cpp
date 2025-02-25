/*
 * tev -- the EXR viewer
 *
 * Copyright (C) 2025 Thomas Müller <contact@tom94.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <tev/Common.h>
#include <tev/ThreadPool.h>
#include <tev/imageio/AppleMakerNote.h>
#include <tev/imageio/GainMap.h>
#include <tev/imageio/HeifImageLoader.h>

#include <libheif/heif.h>

#include <lcms2.h>
#include <lcms2_fast_float.h>

#include <ImfChromaticities.h>

#include <libexif/exif-data.h>

using namespace nanogui;
using namespace std;

namespace tev {

HeifImageLoader::HeifImageLoader() {
    cmsSetLogErrorHandler([](cmsContext, cmsUInt32Number errorCode, const char* message) {
        tlog::error() << fmt::format("lcms error #{}: {}", errorCode, message);
    });

    cmsPlugin(cmsFastFloatExtensions());

    // ExifLog* exifLog = exif_log_new();
    // exif_log_set_func(
    //     exifLog,
    //     [](ExifLog* log, ExifLogCode code, const char* domain, const char* format, va_list args, void* data) {
    //         // sprintf into string
    //         string message;
    //         message.resize(1024);
    //         vsnprintf(message.data(), message.size(), format, args);
    //         switch (code) {
    //             case EXIF_LOG_CODE_NONE: tlog::error() << message; break;
    //             case EXIF_LOG_CODE_DEBUG: tlog::error() << message; break;
    //             case EXIF_LOG_CODE_NO_MEMORY: tlog::error() << message; break;
    //             case EXIF_LOG_CODE_CORRUPT_DATA: tlog::error() << message; break;
    //         }
    //     },
    //     nullptr
    // );
}

bool HeifImageLoader::canLoadFile(istream& iStream) const {
    // libheif's spec says it needs the first 12 bytes to determine whether the image can be read.
    uint8_t header[12];
    iStream.read((char*)header, 12);
    bool failed = !iStream || iStream.gcount() != 12;

    iStream.clear();
    iStream.seekg(0);
    if (failed) {
        return false;
    }

    return heif_check_filetype(header, 12) == heif_filetype_yes_supported;
}

Task<vector<ImageData>> HeifImageLoader::load(istream& iStream, const fs::path&, const string& channelSelector, int priority) const {
    vector<ImageData> result;

    iStream.seekg(0, ios_base::end);
    int64_t fileSize = iStream.tellg();
    iStream.clear();
    iStream.seekg(0);

    struct ReaderContext {
        istream& stream;
        int64_t size;
    } readerContext = {iStream, fileSize};

    static const heif_reader reader = {
        .reader_api_version = 1,
        .get_position = [](void* context) { return (int64_t)reinterpret_cast<ReaderContext*>(context)->stream.tellg(); },
        .read =
            [](void* data, size_t size, void* context) {
                auto& stream = reinterpret_cast<ReaderContext*>(context)->stream;
                stream.read((char*)data, size);
                return stream.good() ? 0 : -1;
            },
        .seek =
            [](int64_t pos, void* context) {
                auto& stream = reinterpret_cast<ReaderContext*>(context)->stream;
                stream.seekg(pos);
                return stream.good() ? 0 : -1;
            },
        .wait_for_file_size =
            [](int64_t target_size, void* context) {
                return reinterpret_cast<ReaderContext*>(context)->size < target_size ? heif_reader_grow_status_size_beyond_eof :
                                                                                       heif_reader_grow_status_size_reached;
            },
        // Not used by API version 1
        .request_range = {},
        .preload_range_hint = {},
        .release_file_range = {},
        .release_error_msg = {},
    };

    heif_context* ctx = heif_context_alloc();
    if (!ctx) {
        throw invalid_argument{"Failed to allocate libheif context."};
    }

    ScopeGuard contextGuard{[ctx] { heif_context_free(ctx); }};

    if (auto error = heif_context_read_from_reader(ctx, &reader, &readerContext, nullptr); error.code != heif_error_Ok) {
        throw invalid_argument{fmt::format("Failed to read image: {}", error.message)};
    }

    // get a handle to the primary image
    heif_image_handle* handle;
    if (auto error = heif_context_get_primary_image_handle(ctx, &handle); error.code != heif_error_Ok) {
        throw invalid_argument{fmt::format("Failed to get primary image handle: {}", error.message)};
    }

    ScopeGuard handleGuard{[handle] { heif_image_handle_release(handle); }};

    auto decodeImage = [priority](heif_image_handle* imgHandle, const Vector2i& targetSize = {0}, const string& namePrefix = "") -> Task<ImageData> {
        ImageData resultData;

        int numChannels = heif_image_handle_has_alpha_channel(imgHandle) ? 4 : 3;
        resultData.hasPremultipliedAlpha = numChannels == 4 && heif_image_handle_is_premultiplied_alpha(imgHandle);

        const bool is_little_endian = std::endian::native == std::endian::little;
        auto format = numChannels == 4 ? (is_little_endian ? heif_chroma_interleaved_RRGGBBAA_LE : heif_chroma_interleaved_RRGGBBAA_BE) :
                                         (is_little_endian ? heif_chroma_interleaved_RRGGBB_LE : heif_chroma_interleaved_RRGGBB_BE);

        Vector2i size = {heif_image_handle_get_width(imgHandle), heif_image_handle_get_height(imgHandle)};

        if (size.x() == 0 || size.y() == 0) {
            throw invalid_argument{"Image has zero pixels."};
        }

        heif_image* img;
        if (auto error = heif_decode_image(imgHandle, &img, heif_colorspace_RGB, format, nullptr); error.code != heif_error_Ok) {
            throw invalid_argument{fmt::format("Failed to decode image: {}", error.message)};
        }

        ScopeGuard imgGuard{[img] { heif_image_release(img); }};

        // if (targetSize.x() != 0 && size != targetSize) {
        //     heif_image* scaledImg;
        //     heif_image_scale_image(img, &scaledImg, targetSize.x(), targetSize.y(), nullptr);
        //     heif_image_release(img);
        //     img = scaledImg;
        // }

        const int bitsPerPixel = heif_image_get_bits_per_pixel_range(img, heif_channel_interleaved);
        const float channelScale = 1.0f / float((1 << bitsPerPixel) - 1);

        int bytesPerLine;
        const uint8_t* data = heif_image_get_plane_readonly(img, heif_channel_interleaved, &bytesPerLine);
        if (!data) {
            throw invalid_argument{"Faild to get image data."};
        }

        resultData.channels = makeNChannels(numChannels, size, namePrefix);

        auto getCmsTransform = [&imgHandle, &numChannels, &resultData]() {
            size_t profileSize = heif_image_handle_get_raw_color_profile_size(imgHandle);
            if (profileSize == 0) {
                return (cmsHTRANSFORM) nullptr;
            }

            vector<uint8_t> profileData(profileSize);
            if (auto error = heif_image_handle_get_raw_color_profile(imgHandle, profileData.data()); error.code != heif_error_Ok) {
                if (error.code == heif_error_Color_profile_does_not_exist) {
                    return (cmsHTRANSFORM) nullptr;
                }

                tlog::warning() << "Failed to read ICC profile: " << error.message;
                return (cmsHTRANSFORM) nullptr;
            }

            // Create ICC profile from the raw data
            cmsHPROFILE srcProfile = cmsOpenProfileFromMem(profileData.data(), (cmsUInt32Number)profileSize);
            if (!srcProfile) {
                tlog::warning() << "Failed to create ICC profile from raw data";
                return (cmsHTRANSFORM) nullptr;
            }

            ScopeGuard srcProfileGuard{[srcProfile] { cmsCloseProfile(srcProfile); }};

            cmsCIExyY D65 = {0.3127, 0.3290, 1.0};
            cmsCIExyYTRIPLE Rec709Primaries = {
                {0.6400, 0.3300, 1.0},
                {0.3000, 0.6000, 1.0},
                {0.1500, 0.0600, 1.0}
            };

            cmsToneCurve* linearCurve[3];
            linearCurve[0] = linearCurve[1] = linearCurve[2] = cmsBuildGamma(0, 1.0f);

            cmsHPROFILE rec709Profile = cmsCreateRGBProfile(&D65, &Rec709Primaries, linearCurve);

            if (!rec709Profile) {
                tlog::warning() << "Failed to create Rec.709 color profile";
                return (cmsHTRANSFORM) nullptr;
            }

            ScopeGuard rec709ProfileGuard{[rec709Profile] { cmsCloseProfile(rec709Profile); }};

            // Create transform from source profile to Rec.709
            auto type = numChannels == 4 ? (resultData.hasPremultipliedAlpha ? TYPE_RGBA_FLT_PREMUL : TYPE_RGBA_FLT) : TYPE_RGB_FLT;
            cmsHTRANSFORM transform = cmsCreateTransform(srcProfile, type, rec709Profile, TYPE_RGBA_FLT, INTENT_PERCEPTUAL, cmsFLAGS_NOCACHE);

            if (!transform) {
                tlog::warning() << "Failed to create color transform from ICC profile to Rec.709";
                return (cmsHTRANSFORM) nullptr;
            }

            return transform;
        };

        // If we've got an ICC color profile, apply that because it's the most detailed / standardized.
        auto transform = getCmsTransform();
        if (transform) {
            ScopeGuard transformGuard{[transform] { cmsDeleteTransform(transform); }};

            tlog::debug() << "Found ICC color profile.";

            // lcms can't perform alpha premultiplication, so we leave it up to downstream processing
            resultData.hasPremultipliedAlpha = false;

            size_t numPixels = (size_t)size.x() * size.y();
            vector<float> src(numPixels * numChannels);
            vector<float> dst(numPixels * numChannels);

            const size_t n_samples_per_row = size.x() * numChannels;
            co_await ThreadPool::global().parallelForAsync<size_t>(
                0,
                size.y(),
                [&](size_t y) {
                    size_t src_offset = y * n_samples_per_row;
                    for (size_t x = 0; x < n_samples_per_row; ++x) {
                        const uint16_t* typedData = reinterpret_cast<const uint16_t*>(data + y * bytesPerLine);
                        src[src_offset + x] = (float)typedData[x] * channelScale;
                    }

                    // Armchair parallelization of lcms: cmsDoTransform is reentrant per the spec, i.e. it can be called from multiple threads.
                    // So: call cmsDoTransform for each row in parallel.
                    // NOTE: This core depends on makeNChannels creating RGBA interleaved buffers!
                    size_t dst_offset = y * (size_t)size.x() * 4;
                    cmsDoTransform(transform, &src[src_offset], &resultData.channels[0].data()[dst_offset], size.x());
                },
                priority
            );

            co_return resultData;
        }

        // Otherwise, assume the image is in Rec.709/sRGB and convert it to linear space, followed by an optional change in color space if
        // an NCLX profile is present.
        co_await ThreadPool::global().parallelForAsync<int>(
            0,
            size.y(),
            [&](int y) {
                for (int x = 0; x < size.x(); ++x) {
                    size_t i = y * (size_t)size.x() + x;
                    auto typedData = reinterpret_cast<const unsigned short*>(data + y * bytesPerLine);
                    int baseIdx = x * numChannels;

                    for (int c = 0; c < numChannels; ++c) {
                        if (c == 3) {
                            resultData.channels[c].at(i) = typedData[baseIdx + c] * channelScale;
                        } else {
                            resultData.channels[c].at(i) = toLinear(typedData[baseIdx + c] * channelScale);
                        }
                    }
                }
            },
            priority
        );

        heif_color_profile_nclx* nclx = nullptr;
        if (auto error = heif_image_handle_get_nclx_color_profile(imgHandle, &nclx); error.code != heif_error_Ok) {
            if (error.code == heif_error_Color_profile_does_not_exist) {
                co_return resultData;
            }

            tlog::warning() << "Failed to read NCLX profile: " << error.message;
            co_return resultData;
        }

        ScopeGuard nclxGuard{[nclx] { heif_nclx_color_profile_free(nclx); }};

        tlog::debug() << "Found NCLX color profile.";

        // Only convert if not already in Rec.709/sRGB
        if (nclx->color_primaries != heif_color_primaries_ITU_R_BT_709_5) {
            Imf::Chromaticities rec709; // default rec709 (sRGB) primaries
            Imf::Chromaticities chroma = {
                {nclx->color_primary_red_x,   nclx->color_primary_red_y  },
                {nclx->color_primary_green_x, nclx->color_primary_green_y},
                {nclx->color_primary_blue_x,  nclx->color_primary_blue_y },
                {nclx->color_primary_white_x, nclx->color_primary_white_y}
            };

            Imath::M44f M = Imf::RGBtoXYZ(chroma, 1) * Imf::XYZtoRGB(rec709, 1);
            for (int m = 0; m < 4; ++m) {
                for (int n = 0; n < 4; ++n) {
                    resultData.toRec709.m[m][n] = M.x[m][n];
                }
            }
        }

        co_return resultData;
    };

    // Read main image
    result.emplace_back(co_await decodeImage(handle));
    ImageData& mainImage = result.front();

    auto findAppleMakerNote = [&]() -> unique_ptr<AppleMakerNote> {
        // Extract EXIF metadata
        int numMetadataBlocks = heif_image_handle_get_number_of_metadata_blocks(handle, "Exif");
        if (numMetadataBlocks <= 0) {
            tlog::warning() << "No EXIF metadata found";
            return nullptr;
        }

        if (numMetadataBlocks > 1) {
            tlog::debug() << "Found " << numMetadataBlocks << " EXIF metadata block(s)";
        }

        vector<heif_item_id> metadataIDs(numMetadataBlocks);
        heif_image_handle_get_list_of_metadata_block_IDs(handle, "Exif", metadataIDs.data(), numMetadataBlocks);

        for (int i = 0; i < numMetadataBlocks; ++i) {
            size_t exifSize = heif_image_handle_get_metadata_size(handle, metadataIDs[i]);
            if (exifSize <= 4) {
                tlog::warning() << "Failed to get size of EXIF data";
                continue;
            }

            vector<uint8_t> exifData(exifSize);
            if (auto error = heif_image_handle_get_metadata(handle, metadataIDs[i], exifData.data()); error.code != heif_error_Ok) {
                tlog::warning() << "Failed to read EXIF data: " << error.message;
                continue;
            }

            ExifData* exif = exif_data_new_from_data(exifData.data() + 4, (unsigned int)(exifSize - 4));
            if (!exif) {
                tlog::warning() << "Failed to decode EXIF data";
                continue;
            }

            ScopeGuard exifGuard{[exif] { exif_data_unref(exif); }};

            ExifEntry* makerNote = exif_data_get_entry(exif, EXIF_TAG_MAKER_NOTE);
            if (!isAppleMakernote(makerNote->data, makerNote->size)) {
                continue;
            }

            return make_unique<AppleMakerNote>(makerNote->data, makerNote->size);
        }

        return nullptr;
    };

    auto resizeImage = [priority](ImageData& resultData, const Vector2i& targetSize, const string& namePrefix) -> Task<void> {
        Vector2i size = resultData.channels.front().size();
        if (size == targetSize) {
            co_return;
        }

        int numChannels = (int)resultData.channels.size();

        ImageData scaledResultData;
        scaledResultData.hasPremultipliedAlpha = resultData.hasPremultipliedAlpha;
        scaledResultData.channels = makeNChannels(numChannels, targetSize, namePrefix);

        auto& srcChannels = resultData.channels;
        auto& dstChannels = scaledResultData.channels;

        co_await ThreadPool::global().parallelForAsync<int>(
            0,
            targetSize.y(),
            [&](int dstY) {
                const float scaleX = (float)size.x() / targetSize.x();
                const float scaleY = (float)size.y() / targetSize.y();

                for (int dstX = 0; dstX < targetSize.x(); ++dstX) {
                    float srcX = (dstX + 0.5f) * scaleX - 0.5f;
                    float srcY = (dstY + 0.5f) * scaleY - 0.5f;

                    int x0 = std::max((int)std::floor(srcX), 0);
                    int y0 = std::max((int)std::floor(srcY), 0);
                    int x1 = std::min(x0 + 1, size.x() - 1);
                    int y1 = std::min(y0 + 1, size.y() - 1);

                    float wx1 = srcX - x0;
                    float wy1 = srcY - y0;
                    float wx0 = 1.0f - wx1;
                    float wy0 = 1.0f - wy1;

                    size_t dstIdx = dstY * (size_t)targetSize.x() + dstX;

                    size_t srcIdx00 = y0 * (size_t)size.x() + x0;
                    size_t srcIdx01 = y0 * (size_t)size.x() + x1;
                    size_t srcIdx10 = y1 * (size_t)size.x() + x0;
                    size_t srcIdx11 = y1 * (size_t)size.x() + x1;

                    for (int c = 0; c < numChannels; ++c) {
                        float p00 = srcChannels[c].at(srcIdx00);
                        float p01 = srcChannels[c].at(srcIdx01);
                        float p10 = srcChannels[c].at(srcIdx10);
                        float p11 = srcChannels[c].at(srcIdx11);

                        float interpolated = wy0 * (wx0 * p00 + wx1 * p01) + wy1 * (wx0 * p10 + wx1 * p11);
                        dstChannels[c].at(dstIdx) = interpolated;
                    }
                }
            },
            priority
        );

        resultData = std::move(scaledResultData);
        co_return;
    };

    // Read auxiliary images
    int num_aux = heif_image_handle_get_number_of_auxiliary_images(handle, 0);
    if (num_aux > 0) {
        tlog::debug() << "Found " << num_aux << " auxiliary image(s)";

        vector<heif_item_id> aux_ids(num_aux);
        heif_image_handle_get_list_of_auxiliary_image_IDs(handle, 0, aux_ids.data(), num_aux);

        for (int i = 0; i < num_aux; ++i) {
            heif_image_handle* auxImgHandle;
            if (auto error = heif_image_handle_get_auxiliary_image_handle(handle, aux_ids[i], &auxImgHandle); error.code != heif_error_Ok) {
                tlog::warning() << fmt::format("Failed to get auxiliary image handle: {}", error.message);
                continue;
            }

            const char* auxType = nullptr;
            if (auto error = heif_image_handle_get_auxiliary_type(auxImgHandle, &auxType); error.code != heif_error_Ok) {
                tlog::warning() << fmt::format("Failed to get auxiliary image type: {}", error.message);
                continue;
            }

            ScopeGuard typeGuard{[auxImgHandle, &auxType] { heif_image_handle_release_auxiliary_type(auxImgHandle, &auxType); }};
            string auxLayerName = auxType ? fmt::format("{}.", auxType) : fmt::format("{}.", num_aux);
            replace(auxLayerName.begin(), auxLayerName.end(), ':', '.');

            if (!matchesFuzzy(auxLayerName, channelSelector)) {
                continue;
            }

            auto auxImgData = co_await decodeImage(auxImgHandle, mainImage.channels.front().size(), auxLayerName);
            co_await resizeImage(auxImgData, mainImage.channels.front().size(), auxLayerName);
            mainImage.channels.insert(mainImage.channels.end(), auxImgData.channels.begin(), auxImgData.channels.end());

            // If we found an apple-style gainmap, apply it to the main image.
            if (auxLayerName.find("apple") != string::npos && auxLayerName.find("hdrgainmap") != string::npos) {
                tlog::debug() << fmt::format("Found hdrgainmap: {}", auxLayerName);
                auto amn = findAppleMakerNote();
                if (amn) {
                    tlog::debug() << "Found Apple maker note; applying gain map.";
                    co_await applyAppleGainMap(
                        result.front(), // primary image
                        auxImgData,
                        priority,
                        *amn
                    );
                }
            }
        }
    }

    co_return result;
}

} // namespace tev
