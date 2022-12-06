/***************************************************************************************************
 * Copyright (c) 2021-2022, NVIDIA CORPORATION. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *  * Neither the name of NVIDIA CORPORATION nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **************************************************************************************************/

#ifndef SHADERS_PLUGIN_OPENIMAGEIO_OPENIMAGEIO_UTILITIES_H
#define SHADERS_PLUGIN_OPENIMAGEIO_OPENIMAGEIO_UTILITIES_H

#include <mi/base.h>

#include <OpenImageIO/imageio.h>

#include <io/image/image/i_image_utilities.h>

namespace mi {
namespace neuraylib {
class IImage_api;
class IReader;
class ITile;
class IWriter;
} // namespace neuraylib
} // namespace mi

namespace MI {

namespace MI_OIIO {

/// The logger used by #log() below. Do not use it directly, because it might be invalid if the
/// plugin API is not available. Use #log() instead.
extern mi::base::Handle<mi::base::ILogger> g_logger;

/// Logs a message.
///
/// The message is discarded if the plugin API and the logger is not available.
void log( mi::base::Message_severity severity, const char* message);

/// Returns the OIIO base type for a \em single channel of our pixel type.
OIIO::TypeDesc::BASETYPE get_base_type( IMAGE::Pixel_type pixel_type);

/// Returns the OIIO image spec for our pixel type and resolution.
OIIO::ImageSpec get_image_spec(
    IMAGE::Pixel_type,
    mi::Uint32 resolution_x,
    mi::Uint32 resolution_y,
    mi::Uint32 resolution_z);

/// Returns our pixel type that should be used for the give image spec.
IMAGE::Pixel_type get_pixel_type( const OIIO::ImageSpec& spec);

/// Premultiplies color channels by alpha channel (as expected by OIIO for export by default).
const mi::neuraylib::ITile* associate_alpha(
    mi::neuraylib::IImage_api* image_api, const mi::neuraylib::ITile* tile, mi::Float32 gamma);

/// Un-premultiplies color channels by alpha channel (as delivered by OIIO upon import by default).
mi::neuraylib::ITile* unassociate_alpha(
    mi::neuraylib::IImage_api* image_api, mi::neuraylib::ITile* tile, mi::Float32 gamma);

/// Wraps a reader into an input proxy.
OIIO::Filesystem::IOProxy* create_input_proxy(
    mi::neuraylib::IReader* reader, bool use_buffer, std::vector<char>& buffer);

/// Wraps a writer into an output proxy. Unused (slower than local buffer).
OIIO::Filesystem::IOProxy* create_output_proxy( mi::neuraylib::IWriter* writer);

/// Expands gray-alpha (YA) data row by row into RGBA data.
///
/// All parameters below can be computed from the corresponding ITile interface.
///
/// \param bpc            Bytes per channel. Implementation supports only bpc == 1, 2, or 4.
/// \param resolution_x   Resolution in x-direction.
/// \param resolution_y   Resolution in y-direction.
/// \param data           Data buffer.
void expand_ya_to_rgba(
    int bpc, mi::Uint32 resolution_x, mi::Uint32 resolution_y, mi::Uint8* data);

} // namespace MI_OIIO

} // namespace MI

#endif // SHADERS_PLUGIN_OPENIMAGEIO_OPENIMAGEIO_UTILITIES_H
