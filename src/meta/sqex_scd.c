#include "meta.h"
#include "../coding/coding.h"
#include "sqex_scd_streamfile.h"


#ifdef VGM_USE_VORBIS
static void scd_ogg_decrypt_v2_callback(void *ptr, size_t size, size_t nmemb, void *datasource, int bytes_read);
static void scd_ogg_decrypt_v3_callback(void *ptr, size_t size, size_t nmemb, void *datasource, int bytes_read);
#endif

/* SCD - Square-Enix console games (FF XIII, XIV) */
VGMSTREAM * init_vgmstream_sqex_scd(STREAMFILE *streamFile) {
    VGMSTREAM * vgmstream = NULL;
    char filename[PATH_LIMIT];
    off_t start_offset, tables_offset, headers_offset, meta_offset, post_meta_offset;
    int headers_entries;
    int32_t stream_size, loop_start, loop_end;

    int target_stream = streamFile->stream_index;
    int loop_flag = 0, channel_count, codec_id, sample_rate;
    int aux_chunk_count;

    int32_t (*read_32bit)(off_t,STREAMFILE*) = NULL;
    int16_t (*read_16bit)(off_t,STREAMFILE*) = NULL;


    /* check extension, case insensitive */
    if ( !check_extensions(streamFile, "scd") ) goto fail;
    streamFile->get_name(streamFile,filename,sizeof(filename));

    /* SEDB */
    if (read_32bitBE(0x00,streamFile) != 0x53454442) goto fail;
    /* SSCF */
    if (read_32bitBE(0x04,streamFile) != 0x53534346) goto fail;

    /** main header section **/
    if (read_32bitBE(0x08,streamFile) == 2 || /* version 2 BE, as seen in FFXIII demo for PS3 */
        read_32bitBE(0x08,streamFile) == 3) { /* version 3 BE, as seen in FFXIII for PS3 */

        read_32bit = read_32bitBE;
        read_16bit = read_16bitBE;
        //size_offset = 0x14;
    } else if (read_32bitLE(0x08,streamFile) == 3 || /* version 2/3 LE, as seen in FFXIV for PC (and others?) */
               read_32bitLE(0x08,streamFile) == 2) {

        read_32bit = read_32bitLE;
        read_16bit = read_16bitLE;
        //size_offset = 0x10;
    } else goto fail;

    /*  0x0c: probably 0=LE, 1=BE */
    /*  0x0d: unk (always 0x04) */
    tables_offset = read_16bit(0xe,streamFile);

#if 0
    /* never mind, FFXIII music_68tak.ps3.scd is 0x80 shorter */
    /* check file size with header value */
    if (read_32bit(size_offset,streamFile) != get_streamfile_size(streamFile))
        goto fail;
#endif

    /** offset tables  **/
    /* 0x00: table1_unknown entries */
    /* 0x02: table2_unknown entries */
    /* 0x04: table_headers entries */
    /* 0x06: unknown (varies) */
    /* 0x08: table1_unknown start offset */
    /* 0x0c: table_headers start offset */
    /* 0x10: table2_unknown start offset */
    /* 0x14: unknown (0x0) */
    /* 0x18: unknown offset */
    /* 0x1c: unknown (0x0)  */
    headers_entries = read_16bit(tables_offset+0x04,streamFile);
    if (target_stream == 0) target_stream = 1; /* auto: default to 1 */
    if (target_stream < 0 || target_stream > headers_entries || headers_entries < 1) goto fail;

    headers_offset = read_32bit(tables_offset+0x0c,streamFile);

    /** header table entries (each is an uint32_t offset to stream header) **/
    meta_offset = read_32bit(headers_offset + (target_stream-1)*4,streamFile);

    /** stream header **/
    stream_size   = read_32bit(meta_offset+0x00,streamFile);
    channel_count = read_32bit(meta_offset+0x04,streamFile);
    sample_rate   = read_32bit(meta_offset+0x08,streamFile);
    codec_id      = read_32bit(meta_offset+0x0c,streamFile);

    loop_start    = read_32bit(meta_offset+0x10,streamFile);
    loop_end      = read_32bit(meta_offset+0x14,streamFile);
    loop_flag     = (loop_end > 0);

    post_meta_offset = meta_offset + 0x20;
    start_offset = post_meta_offset + read_32bit(meta_offset+0x18,streamFile);
    aux_chunk_count = read_32bit(meta_offset+0x1c,streamFile);
    /* 0x01e(e): unknown, seen in some FF XIV sfx (IMA) */

    /* only "MARK" chunk is known (some FF XIV PS3 have "STBL" but it's not counted) */
    if (aux_chunk_count > 1 && aux_chunk_count < 0xFFFF) { /* some FF XIV Heavensward IMA sfx has 0x01000000 */
        VGM_LOG("SCD: unknown aux chunk count %i\n", aux_chunk_count);
        goto fail;
    }

    /* skips aux chunks, sometimes needed (Lightning Returns X360, FF XIV PC) */
    if (aux_chunk_count && read_32bitBE(post_meta_offset, streamFile) == 0x4D41524B) { /* "MARK" */
        post_meta_offset += read_32bit(post_meta_offset+0x04, streamFile);
    }


#ifdef VGM_USE_VORBIS
    /* special case using init_vgmstream_ogg_vorbis with callbacks */
    if (codec_id == 0x06) {
        VGMSTREAM * result = NULL;
        uint32_t seek_table_size, vorb_header_size;
        uint8_t xor_version, xor_byte;
        vgm_vorbis_info_t inf = {0};

        inf.loop_start = loop_start;
        inf.loop_end = loop_end;
        inf.loop_flag = loop_flag;
        inf.loop_end_found = loop_flag;
        inf.loop_length_found = 0;
        inf.layout_type = layout_ogg_vorbis;
        inf.meta_type = meta_SQEX_SCD;

        /* the following could be simplified but it's not clear what field signals that seek table exists
         * (seems that encrypted = always seek table, but maybe post_meta_offset+0x01 = 0x20) */

        /* try regular Ogg with default values */
        {
            result = init_vgmstream_ogg_vorbis_callbacks(streamFile, filename, NULL, start_offset, &inf);
            if (result != NULL)
                return result;
        }

        /* skip seek table and try regular Ogg again */
        {
            seek_table_size  = read_32bit(post_meta_offset+0x10, streamFile);
            vorb_header_size = read_32bit(post_meta_offset+0x14, streamFile);
            if ((post_meta_offset-meta_offset) + seek_table_size + vorb_header_size != read_32bit(meta_offset+0x18, streamFile)) {
                return NULL;
            }

            start_offset = post_meta_offset + 0x20 + seek_table_size;

            result = init_vgmstream_ogg_vorbis_callbacks(streamFile, filename, NULL, start_offset, &inf);
            if (result != NULL)
                return result;
        }

        /* try encrypted Ogg (with seek table already skipped) */
        {
            xor_version = read_8bit(post_meta_offset + 0x00, streamFile);
            xor_byte    = read_8bit(post_meta_offset + 0x02, streamFile);
            if (xor_byte == 0)
                return NULL; /* not actually encrypted, happens but should be handled above */

            if (xor_version == 2) {  /* header is XOR'ed using byte */
                inf.decryption_enabled = 1;
                inf.decryption_callback = scd_ogg_decrypt_v2_callback;
                inf.scd_xor = xor_byte;
                inf.scd_xor_length = vorb_header_size;
            }
            else if (xor_version == 3) { /* full file is XOR'ed using table */
                inf.decryption_enabled = 1;
                inf.decryption_callback = scd_ogg_decrypt_v3_callback;
                inf.scd_xor = stream_size & 0xFF; /* xor_byte is not used? (also there is data at +0x03) */
                inf.scd_xor_length = stream_size;
            }
            else {
                VGM_LOG("SCD: unknown encryption 0x%x\n", xor_version);
                return NULL;
            }

            /* hope this works */
            return init_vgmstream_ogg_vorbis_callbacks(streamFile, filename, NULL, start_offset, &inf);
        }
    }
#endif


    /* build the VGMSTREAM */
    vgmstream = allocate_vgmstream(channel_count,loop_flag);
    if (!vgmstream) goto fail;

    /* fill in the vital statistics */
    vgmstream->channels = channel_count;
    vgmstream->sample_rate = sample_rate;
    vgmstream->num_streams = headers_entries;
    vgmstream->meta_type = meta_SQEX_SCD;

    switch (codec_id) {
        case 0x01:      /* PCM */
            vgmstream->coding_type = coding_PCM16_int;
            vgmstream->layout_type = layout_none;
            vgmstream->num_samples = stream_size / 2 / channel_count;

            if (loop_flag) {
                vgmstream->loop_start_sample = loop_start / 2 / channel_count;
                vgmstream->loop_end_sample = loop_end / 2 / channel_count;
            }
            break;

        case 0x06:      /* OGG [Final Fantasy XIII-2 (PC), Final Fantasy XIV (PC)] */
            goto fail; /* handled above */

#ifdef VGM_USE_MPEG
        case 0x07: {    /* MPEG [Final Fantasy XIII (PS3)] */
            mpeg_codec_data *mpeg_data = NULL;
            mpeg_custom_config cfg = {0};

            cfg.interleave = 0x800; /* for multistream [Final Fantasy XIII-2 (PS3)], otherwise ignored */
            cfg.data_size = stream_size;

            mpeg_data = init_mpeg_custom_codec_data(streamFile, start_offset, &vgmstream->coding_type, vgmstream->channels, MPEG_SCD, &cfg);
            if (!mpeg_data) goto fail;
            vgmstream->codec_data = mpeg_data;
            vgmstream->layout_type = layout_none;

            /* some Drakengard 3, Kingdom Hearts HD have adjusted sample rate (47999, 44099), for looping? */

            vgmstream->num_samples = mpeg_bytes_to_samples(stream_size, mpeg_data);
            vgmstream->loop_start_sample = mpeg_bytes_to_samples(loop_start, mpeg_data);
            vgmstream->loop_end_sample = mpeg_bytes_to_samples(loop_end, mpeg_data);

            //todo find if this actually helps
            vgmstream->num_samples -= vgmstream->num_samples%576;
            vgmstream->loop_start_sample -= vgmstream->loop_start_sample%576;
            vgmstream->loop_end_sample -= vgmstream->loop_end_sample%576;

            break;
        }
#endif
        case 0x0C:      /* MS ADPCM [Final Fantasy XIV (PC) sfx] */
            vgmstream->coding_type = coding_MSADPCM;
            vgmstream->layout_type = layout_none;
            vgmstream->interleave_block_size = read_16bit(post_meta_offset+0x0c,streamFile);

            vgmstream->num_samples = msadpcm_bytes_to_samples(stream_size, vgmstream->interleave_block_size, vgmstream->channels);
            if (loop_flag) {
                vgmstream->loop_start_sample = msadpcm_bytes_to_samples(loop_start, vgmstream->interleave_block_size, vgmstream->channels);
                vgmstream->loop_end_sample = msadpcm_bytes_to_samples(loop_end, vgmstream->interleave_block_size, vgmstream->channels);
            }
            break;

        case 0x0A:      /* DSP ADPCM [Dragon Quest X (Wii)] */
        case 0x15: {    /* DSP ADPCM [Dragon Quest X (Wii U)] (no apparent differences except higher sample rate) */
            STREAMFILE * file;
            int i;
            const off_t interleave_size = 0x800;
            const off_t stride_size = interleave_size * channel_count;
            size_t total_size;
            scd_int_codec_data * data = NULL;


            vgmstream->coding_type = coding_NGC_DSP;
            vgmstream->layout_type = layout_scd_int;

            /* a normal DSP header... */
            total_size = (read_32bitBE(start_offset+0x04,streamFile)+1)/2;
            vgmstream->num_samples = read_32bitBE(start_offset+0x00,streamFile);
            if (loop_flag) {
                vgmstream->loop_start_sample = loop_start;
                vgmstream->loop_end_sample = loop_end+1;
            }

            /* verify other channel headers */
            for (i = 1; i < channel_count; i++) {
                if (read_32bitBE(start_offset+interleave_size*i+0,streamFile) != vgmstream->num_samples ||
                    (read_32bitBE(start_offset+4,streamFile)+1)/2 != total_size) {
                    goto fail;
                }
            }

            /* the primary streamfile we'll be using */
            file = streamFile->open(streamFile,filename,stride_size);
            if (!file) goto fail;

            vgmstream->ch[0].streamfile = file;

            data = malloc(sizeof(scd_int_codec_data));
            data->substream_count = channel_count;
            data->substreams = calloc(channel_count, sizeof(VGMSTREAM *));
            data->intfiles = calloc(channel_count, sizeof(STREAMFILE *));

            vgmstream->codec_data = data;

            for (i=0;i<channel_count;i++) {
                STREAMFILE * intfile =
                    open_scdint_with_STREAMFILE(file, "ARBITRARY.DSP", start_offset+interleave_size*i, interleave_size, stride_size, total_size);
                if (!intfile) goto fail;

                data->substreams[i] = init_vgmstream_ngc_dsp_std(intfile);
                data->intfiles[i] = intfile;
                if (!data->substreams[i]) goto fail;

                /* TODO: only handles mono substreams, though that's all we have with DSP */
                /* save start things so we can restart for seeking/looping */
                memcpy(data->substreams[i]->start_ch,data->substreams[i]->ch,sizeof(VGMSTREAMCHANNEL)*1);
                memcpy(data->substreams[i]->start_vgmstream,data->substreams[i],sizeof(VGMSTREAM));
            }

            break;
        }

#ifdef VGM_USE_FFMPEG
        case 0x0B: {    /* XMA2 [Final Fantasy (X360), Lightning Returns (X360) sfx] */
                ffmpeg_codec_data *ffmpeg_data = NULL;
                uint8_t buf[200];
                int32_t bytes;

                /* post_meta_offset+0x00: fmt0x166 header (BE),  post_meta_offset+0x34: seek table */
                bytes = ffmpeg_make_riff_xma_from_fmt_chunk(buf,200, post_meta_offset,0x34, stream_size, streamFile, 1);
                if (bytes <= 0) goto fail;

                ffmpeg_data = init_ffmpeg_header_offset(streamFile, buf,bytes, start_offset,stream_size);
                if (!ffmpeg_data) goto fail;
                vgmstream->codec_data = ffmpeg_data;
                vgmstream->coding_type = coding_FFmpeg;
                vgmstream->layout_type = layout_none;

                vgmstream->num_samples = ffmpeg_data->totalSamples;
                vgmstream->loop_start_sample = loop_start;
                vgmstream->loop_end_sample = loop_end;
            break;
        }

        case 0x0E: {    /* ATRAC3plus [Lord of Arcana (PSP)] */
            ffmpeg_codec_data *ffmpeg_data = NULL;

            /* full RIFF header at start_offset/post_meta_offset (same) */
            ffmpeg_data = init_ffmpeg_offset(streamFile, start_offset,stream_size);
            if (!ffmpeg_data) goto fail;
            vgmstream->codec_data = ffmpeg_data;
            vgmstream->coding_type = coding_FFmpeg;
            vgmstream->layout_type = layout_none;

            vgmstream->num_samples = ffmpeg_data->totalSamples; /* fact samples */
            vgmstream->loop_start_sample = loop_start;
            vgmstream->loop_end_sample = loop_end;

            /* manually read skip_samples if FFmpeg didn't do it */
            if (ffmpeg_data->skipSamples <= 0) {
                off_t chunk_offset;
                size_t chunk_size, fact_skip_samples = 0;
                if (!find_chunk_le(streamFile, 0x66616374,start_offset+0xc,0, &chunk_offset,&chunk_size)) /* find "fact" */
                    goto fail;
                if (chunk_size == 0x8) {
                    fact_skip_samples  = read_32bitLE(chunk_offset+0x4, streamFile);
                } else if (chunk_size == 0xc) {
                    fact_skip_samples  = read_32bitLE(chunk_offset+0x8, streamFile);
                }
                ffmpeg_set_skip_samples(ffmpeg_data, fact_skip_samples);
            }
            /* SCD loop/sample values are relative (without skip samples) vs RIFF (with skip samples), no need to adjust */

            break;
        }
#endif

        default:
            VGM_LOG("SCD: unknown codec_id 0x%x\n", codec_id);
            goto fail;
    }


    if ( !vgmstream_open_stream(vgmstream, streamFile, start_offset) )
        goto fail;

    return vgmstream;

fail:
    close_vgmstream(vgmstream);
    return NULL;
}


#ifdef VGM_USE_VORBIS
static void scd_ogg_decrypt_v2_callback(void *ptr, size_t size, size_t nmemb, void *datasource, int bytes_read) {
    ogg_vorbis_streamfile * ov_streamfile = (ogg_vorbis_streamfile*)datasource;

    /* header is XOR'd with a constant byte */
    if (ov_streamfile->offset < ov_streamfile->scd_xor_length) {
        int i, num_crypt;

        num_crypt = ov_streamfile->scd_xor_length - ov_streamfile->offset;
        if (num_crypt > bytes_read)
            num_crypt = bytes_read;

        for (i = 0; i < num_crypt; i++) {
            ((uint8_t*)ptr)[i] ^= (uint8_t)ov_streamfile->scd_xor;
        }
    }
}

static void scd_ogg_decrypt_v3_callback(void *ptr, size_t size, size_t nmemb, void *datasource, int bytes_read) {
    /* V3 decryption table found in the .exe */
    static const uint8_t scd_ogg_v3_lookuptable[256] = { /* FF XIV Heavensward */
        0x3A, 0x32, 0x32, 0x32, 0x03, 0x7E, 0x12, 0xF7, 0xB2, 0xE2, 0xA2, 0x67, 0x32, 0x32, 0x22, 0x32, // 00-0F
        0x32, 0x52, 0x16, 0x1B, 0x3C, 0xA1, 0x54, 0x7B, 0x1B, 0x97, 0xA6, 0x93, 0x1A, 0x4B, 0xAA, 0xA6, // 10-1F
        0x7A, 0x7B, 0x1B, 0x97, 0xA6, 0xF7, 0x02, 0xBB, 0xAA, 0xA6, 0xBB, 0xF7, 0x2A, 0x51, 0xBE, 0x03, // 20-2F
        0xF4, 0x2A, 0x51, 0xBE, 0x03, 0xF4, 0x2A, 0x51, 0xBE, 0x12, 0x06, 0x56, 0x27, 0x32, 0x32, 0x36, // 30-3F
        0x32, 0xB2, 0x1A, 0x3B, 0xBC, 0x91, 0xD4, 0x7B, 0x58, 0xFC, 0x0B, 0x55, 0x2A, 0x15, 0xBC, 0x40, // 40-4F
        0x92, 0x0B, 0x5B, 0x7C, 0x0A, 0x95, 0x12, 0x35, 0xB8, 0x63, 0xD2, 0x0B, 0x3B, 0xF0, 0xC7, 0x14, // 50-5F
        0x51, 0x5C, 0x94, 0x86, 0x94, 0x59, 0x5C, 0xFC, 0x1B, 0x17, 0x3A, 0x3F, 0x6B, 0x37, 0x32, 0x32, // 60-6F
        0x30, 0x32, 0x72, 0x7A, 0x13, 0xB7, 0x26, 0x60, 0x7A, 0x13, 0xB7, 0x26, 0x50, 0xBA, 0x13, 0xB4, // 70-7F
        0x2A, 0x50, 0xBA, 0x13, 0xB5, 0x2E, 0x40, 0xFA, 0x13, 0x95, 0xAE, 0x40, 0x38, 0x18, 0x9A, 0x92, // 80-8F
        0xB0, 0x38, 0x00, 0xFA, 0x12, 0xB1, 0x7E, 0x00, 0xDB, 0x96, 0xA1, 0x7C, 0x08, 0xDB, 0x9A, 0x91, // 90-9F
        0xBC, 0x08, 0xD8, 0x1A, 0x86, 0xE2, 0x70, 0x39, 0x1F, 0x86, 0xE0, 0x78, 0x7E, 0x03, 0xE7, 0x64, // A0-AF
        0x51, 0x9C, 0x8F, 0x34, 0x6F, 0x4E, 0x41, 0xFC, 0x0B, 0xD5, 0xAE, 0x41, 0xFC, 0x0B, 0xD5, 0xAE, // B0-BF
        0x41, 0xFC, 0x3B, 0x70, 0x71, 0x64, 0x33, 0x32, 0x12, 0x32, 0x32, 0x36, 0x70, 0x34, 0x2B, 0x56, // C0-CF
        0x22, 0x70, 0x3A, 0x13, 0xB7, 0x26, 0x60, 0xBA, 0x1B, 0x94, 0xAA, 0x40, 0x38, 0x00, 0xFA, 0xB2, // D0-DF
        0xE2, 0xA2, 0x67, 0x32, 0x32, 0x12, 0x32, 0xB2, 0x32, 0x32, 0x32, 0x32, 0x75, 0xA3, 0x26, 0x7B, // E0-EF
        0x83, 0x26, 0xF9, 0x83, 0x2E, 0xFF, 0xE3, 0x16, 0x7D, 0xC0, 0x1E, 0x63, 0x21, 0x07, 0xE3, 0x01, // F0-FF
    };
    ogg_vorbis_streamfile *ov_streamfile = (ogg_vorbis_streamfile*)datasource;

    /* file is XOR'd with a table (algorithm and table by Ioncannon) */
    if (ov_streamfile->offset < ov_streamfile->scd_xor_length) {
        int i, num_crypt;
        uint8_t byte1, byte2, xorByte;

        num_crypt = bytes_read;
        byte1 = ov_streamfile->scd_xor & 0x7F;
        byte2 = ov_streamfile->scd_xor & 0x3F;

        for (i = 0; i < num_crypt; i++) {
            xorByte = scd_ogg_v3_lookuptable[(byte2 + ov_streamfile->offset + i) & 0xFF];
            xorByte &= 0xFF;
            xorByte ^= ((uint8_t*)ptr)[i];
            xorByte ^= byte1;
            ((uint8_t*)ptr)[i] = (uint8_t)xorByte;
        }
    }
}
#endif
