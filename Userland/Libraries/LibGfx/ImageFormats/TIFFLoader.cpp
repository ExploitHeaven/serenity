/*
 * Copyright (c) 2023, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TIFFLoader.h"
#include <AK/ConstrainedStream.h>
#include <AK/Debug.h>
#include <AK/Endian.h>
#include <AK/String.h>
#include <LibCompress/LZWDecoder.h>
#include <LibCompress/PackBitsDecoder.h>
#include <LibCompress/Zlib.h>
#include <LibGfx/ImageFormats/CCITTDecoder.h>
#include <LibGfx/ImageFormats/TIFFMetadata.h>

namespace Gfx {

namespace TIFF {

class TIFFLoadingContext {
public:
    enum class State {
        NotDecoded = 0,
        Error,
        HeaderDecoded,
        FrameDecoded,
    };

    TIFFLoadingContext(NonnullOwnPtr<FixedMemoryStream> stream)
        : m_stream(move(stream))
    {
    }

    ErrorOr<void> decode_image_header()
    {
        TRY(read_image_file_header());
        TRY(read_next_image_file_directory());

        m_state = State::HeaderDecoded;
        return {};
    }

    ErrorOr<void> ensure_baseline_tags_correctness() const
    {
        if (m_metadata.strip_offsets()->size() != m_metadata.strip_byte_counts()->size())
            return Error::from_string_literal("TIFFImageDecoderPlugin: StripsOffset and StripByteCount have different sizes");

        if (any_of(*m_metadata.bits_per_sample(), [](auto bit_depth) { return bit_depth == 0 || bit_depth > 32; }))
            return Error::from_string_literal("TIFFImageDecoderPlugin: Invalid value in BitsPerSample");

        return {};
    }

    ErrorOr<void> decode_frame()
    {
        TRY(ensure_baseline_tags_presence(m_metadata));
        TRY(ensure_baseline_tags_correctness());
        auto maybe_error = decode_frame_impl();

        if (maybe_error.is_error()) {
            m_state = State::Error;
            return maybe_error.release_error();
        }

        return {};
    }

    IntSize size() const
    {
        return { *m_metadata.image_width(), *m_metadata.image_height() };
    }

    Metadata const& metadata() const
    {
        return m_metadata;
    }

    State state() const
    {
        return m_state;
    }

    RefPtr<Bitmap> bitmap() const
    {
        return m_bitmap;
    }

private:
    enum class ByteOrder {
        LittleEndian,
        BigEndian,
    };

    static ErrorOr<u8> read_component(BigEndianInputBitStream& stream, u8 bits)
    {
        // FIXME: This function truncates everything to 8-bits
        auto const value = TRY(stream.read_bits<u32>(bits));

        if (bits > 8)
            return value >> (bits - 8);
        return NumericLimits<u8>::max() * value / ((1 << bits) - 1);
    }

    u8 samples_for_photometric_interpretation() const
    {
        switch (*m_metadata.photometric_interpretation()) {
        case PhotometricInterpretation::WhiteIsZero:
        case PhotometricInterpretation::BlackIsZero:
        case PhotometricInterpretation::RGBPalette:
            return 1;
        case PhotometricInterpretation::RGB:
            return 3;
        default:
            TODO();
        }
    }

    Optional<u8> alpha_channel_index() const
    {
        if (m_metadata.extra_samples().has_value()) {
            auto const extra_samples = m_metadata.extra_samples().value();
            for (u8 i = 0; i < extra_samples.size(); ++i) {
                if (extra_samples[i] == ExtraSample::UnassociatedAlpha)
                    return i + samples_for_photometric_interpretation();
            }
        }
        return OptionalNone {};
    }

    ErrorOr<Color> read_color(BigEndianInputBitStream& stream)
    {
        auto bits_per_sample = *m_metadata.bits_per_sample();

        // Section 7: Additional Baseline TIFF Requirements
        // Some TIFF files may have more components per pixel than you think. A Baseline TIFF reader must skip over
        // them gracefully, using the values of the SamplesPerPixel and BitsPerSample fields.
        auto manage_extra_channels = [&]() -> ErrorOr<u8> {
            // Both unknown and alpha channels are considered as extra channels, so let's iterate over
            // them, conserve the alpha value (if any) and discard everything else.

            auto const number_base_channels = samples_for_photometric_interpretation();
            auto const alpha_index = alpha_channel_index();

            Optional<u8> alpha {};

            for (u8 i = number_base_channels; i < bits_per_sample.size(); ++i) {
                if (alpha_index == i)
                    alpha = TRY(read_component(stream, bits_per_sample[i]));
                else
                    TRY(read_component(stream, bits_per_sample[i]));
            }

            return alpha.value_or(NumericLimits<u8>::max());
        };

        if (m_metadata.photometric_interpretation() == PhotometricInterpretation::RGB) {
            auto const first_component = TRY(read_component(stream, bits_per_sample[0]));
            auto const second_component = TRY(read_component(stream, bits_per_sample[1]));
            auto const third_component = TRY(read_component(stream, bits_per_sample[2]));

            auto const alpha = TRY(manage_extra_channels());
            return Color(first_component, second_component, third_component, alpha);
        }

        if (m_metadata.photometric_interpretation() == PhotometricInterpretation::RGBPalette) {
            auto const index = TRY(stream.read_bits<u16>(bits_per_sample[0]));
            auto const alpha = TRY(manage_extra_channels());

            // SamplesPerPixel == 1 is a requirement for RGBPalette
            // From description of PhotometricInterpretation in Section 8: Baseline Field Reference Guide
            // "In a TIFF ColorMap, all the Red values come first, followed by the Green values,
            //  then the Blue values."
            auto const size = 1 << (*m_metadata.bits_per_sample())[0];
            auto const red_offset = 0 * size;
            auto const green_offset = 1 * size;
            auto const blue_offset = 2 * size;

            auto const color_map = *m_metadata.color_map();

            // FIXME: ColorMap's values are always 16-bits, stop truncating them when we support 16 bits bitmaps
            return Color(
                color_map[red_offset + index] >> 8,
                color_map[green_offset + index] >> 8,
                color_map[blue_offset + index] >> 8,
                alpha);
        }

        if (*m_metadata.photometric_interpretation() == PhotometricInterpretation::WhiteIsZero
            || *m_metadata.photometric_interpretation() == PhotometricInterpretation::BlackIsZero) {
            auto luminosity = TRY(read_component(stream, bits_per_sample[0]));

            if (m_metadata.photometric_interpretation() == PhotometricInterpretation::WhiteIsZero)
                luminosity = ~luminosity;

            auto const alpha = TRY(manage_extra_channels());
            return Color(luminosity, luminosity, luminosity, alpha);
        }

        return Error::from_string_literal("Unsupported value for PhotometricInterpretation");
    }

    template<CallableAs<ErrorOr<ReadonlyBytes>, u32> StripDecoder>
    ErrorOr<void> loop_over_pixels(StripDecoder&& strip_decoder)
    {
        auto const strips_offset = *m_metadata.strip_offsets();
        auto const strip_byte_counts = *m_metadata.strip_byte_counts();

        for (u32 strip_index = 0; strip_index < strips_offset.size(); ++strip_index) {
            TRY(m_stream->seek(strips_offset[strip_index]));

            auto const decoded_bytes = TRY(strip_decoder(strip_byte_counts[strip_index]));
            auto decoded_strip = make<FixedMemoryStream>(decoded_bytes);
            auto decoded_stream = make<BigEndianInputBitStream>(move(decoded_strip));

            for (u32 row = 0; row < *m_metadata.rows_per_strip(); row++) {
                auto const scanline = row + *m_metadata.rows_per_strip() * strip_index;
                if (scanline >= *m_metadata.image_height())
                    break;

                Optional<Color> last_color {};

                for (u32 column = 0; column < *m_metadata.image_width(); ++column) {
                    auto color = TRY(read_color(*decoded_stream));

                    if (m_metadata.predictor() == Predictor::HorizontalDifferencing && last_color.has_value()) {
                        color.set_red(last_color->red() + color.red());
                        color.set_green(last_color->green() + color.green());
                        color.set_blue(last_color->blue() + color.blue());
                    }

                    last_color = color;
                    m_bitmap->set_pixel(column, scanline, color);
                }

                decoded_stream->align_to_byte_boundary();
            }
        }

        return {};
    }

    ErrorOr<void> decode_frame_impl()
    {
        m_bitmap = TRY(Bitmap::create(BitmapFormat::BGRA8888, size()));

        switch (*m_metadata.compression()) {
        case Compression::NoCompression: {
            auto identity = [&](u32 num_bytes) {
                return m_stream->read_in_place<u8 const>(num_bytes);
            };

            TRY(loop_over_pixels(move(identity)));
            break;
        }
        case Compression::CCITT: {
            if (m_metadata.bits_per_sample()->size() > 1)
                return Error::from_string_literal("TIFFImageDecoderPlugin: CCITT image with BitsPerSample greater than one, aborting...");

            ByteBuffer decoded_bytes {};
            auto decode_ccitt_1D_strip = [&](u32 num_bytes) -> ErrorOr<ReadonlyBytes> {
                auto const encoded_bytes = TRY(m_stream->read_in_place<u8 const>(num_bytes));
                decoded_bytes = TRY(CCITT::decode_ccitt3_1d(encoded_bytes, *m_metadata.image_width(), *m_metadata.rows_per_strip()));
                return decoded_bytes;
            };

            TRY(loop_over_pixels(move(decode_ccitt_1D_strip)));
            break;
        }
        case Compression::LZW: {
            ByteBuffer decoded_bytes {};
            auto decode_lzw_strip = [&](u32 num_bytes) -> ErrorOr<ReadonlyBytes> {
                auto const encoded_bytes = TRY(m_stream->read_in_place<u8 const>(num_bytes));

                if (encoded_bytes.is_empty())
                    return Error::from_string_literal("TIFFImageDecoderPlugin: Unable to read from empty LZW strip");

                // Note: AFAIK, there are two common ways to use LZW compression:
                //          - With a LittleEndian stream and no Early-Change, this is used in the GIF format
                //          - With a BigEndian stream and an EarlyChange of 1, this is used in the PDF format
                //       The fun begins when they decided to change from the former to the latter when moving
                //       from TIFF 5.0 to 6.0, and without including a way for files to be identified.
                //       Fortunately, as the first byte of a LZW stream is a constant we can guess the endianess
                //       and deduce the version from it. The first code is 0x100 (9-bits).
                if (encoded_bytes[0] == 0x00)
                    decoded_bytes = TRY(Compress::LZWDecoder<LittleEndianInputBitStream>::decode_all(encoded_bytes, 8, 0));
                else
                    decoded_bytes = TRY(Compress::LZWDecoder<BigEndianInputBitStream>::decode_all(encoded_bytes, 8, -1));

                return decoded_bytes;
            };

            TRY(loop_over_pixels(move(decode_lzw_strip)));
            break;
        }
        case Compression::AdobeDeflate: {
            // This is an extension from the Technical Notes from 2002:
            // https://web.archive.org/web/20160305055905/http://partners.adobe.com/public/developer/en/tiff/TIFFphotoshop.pdf
            ByteBuffer decoded_bytes {};
            auto decode_zlib = [&](u32 num_bytes) -> ErrorOr<ReadonlyBytes> {
                auto stream = make<ConstrainedStream>(MaybeOwned<Stream>(*m_stream), num_bytes);
                auto decompressed_stream = TRY(Compress::ZlibDecompressor::create(move(stream)));
                decoded_bytes = TRY(decompressed_stream->read_until_eof(4096));
                return decoded_bytes;
            };

            TRY(loop_over_pixels(move(decode_zlib)));
            break;
        }
        case Compression::PackBits: {
            // Section 9: PackBits Compression
            ByteBuffer decoded_bytes {};

            auto decode_packbits_strip = [&](u32 num_bytes) -> ErrorOr<ReadonlyBytes> {
                auto const encoded_bytes = TRY(m_stream->read_in_place<u8 const>(num_bytes));
                decoded_bytes = TRY(Compress::PackBits::decode_all(encoded_bytes));
                return decoded_bytes;
            };

            TRY(loop_over_pixels(move(decode_packbits_strip)));
            break;
        }
        default:
            return Error::from_string_literal("This compression type is not supported yet :^)");
        }

        return {};
    }

    template<typename T>
    ErrorOr<T> read_value()
    {
        if (m_byte_order == ByteOrder::LittleEndian)
            return TRY(m_stream->read_value<LittleEndian<T>>());
        if (m_byte_order == ByteOrder::BigEndian)
            return TRY(m_stream->read_value<BigEndian<T>>());
        VERIFY_NOT_REACHED();
    }

    ErrorOr<void> read_next_idf_offset()
    {
        auto const next_block_position = TRY(read_value<u32>());

        if (next_block_position != 0)
            m_next_ifd = Optional<u32> { next_block_position };
        else
            m_next_ifd = OptionalNone {};
        dbgln_if(TIFF_DEBUG, "Setting image file directory pointer to {}", m_next_ifd);
        return {};
    }

    ErrorOr<void> read_image_file_header()
    {
        // Section 2: TIFF Structure - Image File Header

        auto const byte_order = TRY(m_stream->read_value<u16>());

        switch (byte_order) {
        case 0x4949:
            m_byte_order = ByteOrder::LittleEndian;
            break;
        case 0x4D4D:
            m_byte_order = ByteOrder::BigEndian;
            break;
        default:
            return Error::from_string_literal("TIFFImageDecoderPlugin: Invalid byte order");
        }

        auto const magic_number = TRY(read_value<u16>());

        if (magic_number != 42)
            return Error::from_string_literal("TIFFImageDecoderPlugin: Invalid magic number");

        TRY(read_next_idf_offset());

        return {};
    }

    ErrorOr<void> read_next_image_file_directory()
    {
        // Section 2: TIFF Structure - Image File Directory

        if (!m_next_ifd.has_value())
            return Error::from_string_literal("TIFFImageDecoderPlugin: Missing an Image File Directory");

        TRY(m_stream->seek(m_next_ifd.value()));

        auto const number_of_field = TRY(read_value<u16>());

        for (u16 i = 0; i < number_of_field; ++i)
            TRY(read_tag());

        TRY(read_next_idf_offset());
        return {};
    }

    ErrorOr<Type> read_type()
    {
        switch (TRY(read_value<u16>())) {
        case to_underlying(Type::Byte):
            return Type::Byte;
        case to_underlying(Type::ASCII):
            return Type::ASCII;
        case to_underlying(Type::UnsignedShort):
            return Type::UnsignedShort;
        case to_underlying(Type::UnsignedLong):
            return Type::UnsignedLong;
        case to_underlying(Type::UnsignedRational):
            return Type::UnsignedRational;
        case to_underlying(Type::Undefined):
            return Type::Undefined;
        case to_underlying(Type::SignedLong):
            return Type::SignedLong;
        case to_underlying(Type::SignedRational):
            return Type::SignedRational;
        case to_underlying(Type::UTF8):
            return Type::UTF8;
        default:
            return Error::from_string_literal("TIFFImageDecoderPlugin: Unknown type");
        }
    }

    static constexpr u8 size_of_type(Type type)
    {
        switch (type) {
        case Type::Byte:
            return 1;
        case Type::ASCII:
            return 1;
        case Type::UnsignedShort:
            return 2;
        case Type::UnsignedLong:
            return 4;
        case Type::UnsignedRational:
            return 8;
        case Type::Undefined:
            return 1;
        case Type::SignedLong:
            return 4;
        case Type::SignedRational:
            return 8;
        case Type::Float:
            return 4;
        case Type::Double:
            return 8;
        case Type::UTF8:
            return 1;
        default:
            VERIFY_NOT_REACHED();
        }
    }

    ErrorOr<Vector<Value, 1>> read_tiff_value(Type type, u32 count, u32 offset)
    {
        auto const old_offset = TRY(m_stream->tell());
        ScopeGuard reset_offset { [this, old_offset]() { MUST(m_stream->seek(old_offset)); } };

        TRY(m_stream->seek(offset));

        if (size_of_type(type) * count > m_stream->remaining())
            return Error::from_string_literal("TIFFImageDecoderPlugin: Tag size claims to be bigger that remaining bytes");

        auto const read_every_values = [this, count]<typename T>() -> ErrorOr<Vector<Value>> {
            Vector<Value, 1> result {};
            TRY(result.try_ensure_capacity(count));
            if constexpr (IsSpecializationOf<T, Rational>) {
                for (u32 i = 0; i < count; ++i)
                    result.empend(T { TRY(read_value<typename T::Type>()), TRY(read_value<typename T::Type>()) });
            } else {
                for (u32 i = 0; i < count; ++i)
                    result.empend(typename TypePromoter<T>::Type(TRY(read_value<T>())));
            }
            return result;
        };

        switch (type) {
        case Type::Byte:
        case Type::Undefined: {
            Vector<Value, 1> result;
            auto buffer = TRY(ByteBuffer::create_uninitialized(count));
            TRY(m_stream->read_until_filled(buffer));
            result.append(move(buffer));
            return result;
        }
        case Type::ASCII:
        case Type::UTF8: {
            Vector<Value, 1> result;
            // NOTE: No need to include the null terminator
            if (count > 0)
                --count;
            auto string_data = TRY(ByteBuffer::create_uninitialized(count));
            TRY(m_stream->read_until_filled(string_data));
            result.empend(TRY(String::from_utf8(StringView { string_data.bytes() })));
            return result;
        }
        case Type::UnsignedShort:
            return read_every_values.template operator()<u16>();
        case Type::UnsignedLong:
            return read_every_values.template operator()<u32>();
        case Type::UnsignedRational:
            return read_every_values.template operator()<Rational<u32>>();
        case Type::SignedLong:
            return read_every_values.template operator()<i32>();
            ;
        case Type::SignedRational:
            return read_every_values.template operator()<Rational<i32>>();
        default:
            VERIFY_NOT_REACHED();
        }
    }

    ErrorOr<void> read_tag()
    {
        auto const tag = TRY(read_value<u16>());
        auto const type = TRY(read_type());
        auto const count = TRY(read_value<u32>());

        Checked<u32> checked_size = size_of_type(type);
        checked_size *= count;

        if (checked_size.has_overflow())
            return Error::from_string_literal("TIFFImageDecoderPlugin: Invalid tag with too large data");

        auto tiff_value = TRY(([=, this]() -> ErrorOr<Vector<Value>> {
            if (checked_size.value() <= 4) {
                auto value = TRY(read_tiff_value(type, count, TRY(m_stream->tell())));
                TRY(m_stream->discard(4));
                return value;
            }
            auto const offset = TRY(read_value<u32>());
            return read_tiff_value(type, count, offset);
        }()));

        TRY(handle_tag(m_metadata, tag, type, count, move(tiff_value)));

        return {};
    }

    NonnullOwnPtr<FixedMemoryStream> m_stream;
    State m_state {};
    RefPtr<Bitmap> m_bitmap {};

    ByteOrder m_byte_order {};
    Optional<u32> m_next_ifd {};

    Metadata m_metadata {};
};

}

TIFFImageDecoderPlugin::TIFFImageDecoderPlugin(NonnullOwnPtr<FixedMemoryStream> stream)
{
    m_context = make<TIFF::TIFFLoadingContext>(move(stream));
}

bool TIFFImageDecoderPlugin::sniff(ReadonlyBytes bytes)
{
    if (bytes.size() < 4)
        return false;
    bool const valid_little_endian = bytes[0] == 0x49 && bytes[1] == 0x49 && bytes[2] == 0x2A && bytes[3] == 0x00;
    bool const valid_big_endian = bytes[0] == 0x4D && bytes[1] == 0x4D && bytes[2] == 0x00 && bytes[3] == 0x2A;
    return valid_little_endian || valid_big_endian;
}

IntSize TIFFImageDecoderPlugin::size()
{
    return m_context->size();
}

ErrorOr<NonnullOwnPtr<ImageDecoderPlugin>> TIFFImageDecoderPlugin::create(ReadonlyBytes data)
{
    auto stream = TRY(try_make<FixedMemoryStream>(data));
    auto plugin = TRY(adopt_nonnull_own_or_enomem(new (nothrow) TIFFImageDecoderPlugin(move(stream))));
    TRY(plugin->m_context->decode_image_header());
    return plugin;
}

ErrorOr<ImageFrameDescriptor> TIFFImageDecoderPlugin::frame(size_t index, Optional<IntSize>)
{
    if (index > 0)
        return Error::from_string_literal("TIFFImageDecoderPlugin: Invalid frame index");

    if (m_context->state() == TIFF::TIFFLoadingContext::State::Error)
        return Error::from_string_literal("TIFFImageDecoderPlugin: Decoding failed");

    if (m_context->state() < TIFF::TIFFLoadingContext::State::FrameDecoded)
        TRY(m_context->decode_frame());

    return ImageFrameDescriptor { m_context->bitmap(), 0 };
}

ErrorOr<Optional<ReadonlyBytes>> TIFFImageDecoderPlugin::icc_data()
{
    return m_context->metadata().icc_profile().map([](auto const& buffer) -> ReadonlyBytes { return buffer.bytes(); });
}

}
