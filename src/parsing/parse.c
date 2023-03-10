#include "parse.h"

#include "src/sist.h"
#include "src/ctx.h"
#include "mime.h"
#include "src/io/serialize.h"
#include "src/parsing/sidecar.h"
#include "src/magic_generated.c"

#include <magic.h>


#define MIN_VIDEO_SIZE (1024 * 64)
#define MIN_IMAGE_SIZE (512)

int fs_read(struct vfile *f, void *buf, size_t size) {

    if (f->fd == -1) {
        SHA1_Init(&f->sha1_ctx);

        f->fd = open(f->filepath, O_RDONLY);
        if (f->fd == -1) {
            return -1;
        }
    }

    int ret = (int) read(f->fd, buf, size);

    if (ret != 0 && f->calculate_checksum) {
        f->has_checksum = TRUE;
        safe_sha1_update(&f->sha1_ctx, (unsigned char *) buf, ret);
    }

    return ret;
}

#define CLOSE_FILE(f) if ((f).close != NULL) {(f).close(&(f));};

void fs_close(struct vfile *f) {
    if (f->fd != -1) {
        SHA1_Final(f->sha1_digest, &f->sha1_ctx);
        close(f->fd);
    }
}

void fs_reset(struct vfile *f) {
    if (f->fd != -1) {
        lseek(f->fd, 0, SEEK_SET);
    }
}

void set_dbg_current_file(parse_job_t *job) {
    unsigned long long pid = (unsigned long long) pthread_self();
    pthread_mutex_lock(&ScanCtx.dbg_current_files_mu);
    g_hash_table_replace(ScanCtx.dbg_current_files, GINT_TO_POINTER(pid), job);
    pthread_mutex_unlock(&ScanCtx.dbg_current_files_mu);
}

void parse_job(parse_job_t *job) {
    tpool_work_arg_shm_t *arg = malloc(sizeof(tpool_work_arg_shm_t) + sizeof(*job));

    memcpy(arg->arg, job, sizeof(*job));
    arg->arg_size = -1;

    parse(arg);

    free(arg);
}

void parse(tpool_work_arg_shm_t *arg) {

    parse_job_t *job = (void*)arg->arg;

    document_t *doc = malloc(sizeof(document_t));

    set_dbg_current_file(job);

    strcpy(doc->filepath, job->filepath);
    doc->ext = (short) job->ext;
    doc->base = (short) job->base;

    char *rel_path = doc->filepath + ScanCtx.index.desc.root_len;
    generate_doc_id(rel_path, doc->doc_id);

    doc->meta_head = NULL;
    doc->meta_tail = NULL;
    doc->mime = 0;
    doc->size = job->vfile.st_size;
    doc->mtime = (int) job->vfile.mtime;

    int inc_ts = incremental_get(ScanCtx.original_table, doc->doc_id);
    if (inc_ts != 0 && inc_ts == job->vfile.mtime) {
        pthread_mutex_lock(&ScanCtx.copy_table_mu);
        incremental_mark_file(ScanCtx.copy_table, doc->doc_id);
        pthread_mutex_unlock(&ScanCtx.copy_table_mu);

        pthread_mutex_lock(&ScanCtx.dbg_file_counts_mu);
        ScanCtx.dbg_skipped_files_count += 1;
        pthread_mutex_unlock(&ScanCtx.dbg_file_counts_mu);

        CLOSE_FILE(job->vfile)
        free(doc);

        return;
    }

    if (ScanCtx.new_table != NULL) {
        pthread_mutex_lock(&ScanCtx.copy_table_mu);
        incremental_mark_file(ScanCtx.new_table, doc->doc_id);
        pthread_mutex_unlock(&ScanCtx.copy_table_mu);
    }

    char *buf[MAGIC_BUF_SIZE];

    if (LogCtx.very_verbose) {
        LOG_DEBUGF(job->filepath, "Starting parse job {%s}", doc->doc_id)
    }

    if (job->ext > 4096) {
        fprintf(stderr, "Ext is %d, filename is %s\n", job->ext, job->filepath);
    }

    if (job->vfile.st_size == 0) {
        doc->mime = MIME_EMPTY;
    } else if (*(job->filepath + job->ext) != '\0' && (job->ext - job->base != 1)) {
        doc->mime = mime_get_mime_by_ext(ScanCtx.ext_table, job->filepath + job->ext);
    }

    if (doc->mime == 0 && !ScanCtx.fast) {

        // Get mime type with libmagic
        if (job->vfile.read_rewindable == NULL) {
            LOG_WARNING(job->filepath,
                        "File does not support rewindable reads, cannot guess Media type");
            goto abort;
        }

        int bytes_read = job->vfile.read_rewindable(&job->vfile, buf, MAGIC_BUF_SIZE);
        if (bytes_read < 0) {

            if (job->vfile.is_fs_file) {
                LOG_ERRORF(job->filepath, "read(): [%d] %s", errno, strerror(errno))
            } else {
                LOG_ERRORF(job->filepath, "(virtual) read(): [%d] %s", bytes_read, archive_error_string(job->vfile.arc))
            }

            pthread_mutex_lock(&ScanCtx.dbg_file_counts_mu);
            ScanCtx.dbg_failed_files_count += 1;
            pthread_mutex_unlock(&ScanCtx.dbg_file_counts_mu);

            CLOSE_FILE(job->vfile)
            free(doc);

            return;
        }

        magic_t magic = magic_open(MAGIC_MIME_TYPE);

        const char *magic_buffers[1] = {magic_database_buffer,};
        size_t sizes[1] = {sizeof(magic_database_buffer),};

        int load_ret = magic_load_buffers(magic, (void **) &magic_buffers, sizes, 1);

        if (load_ret != 0) {
            LOG_FATALF("parse.c", "Could not load libmagic database: (%d)", load_ret)
        }

        const char *magic_mime_str = magic_buffer(magic, buf, bytes_read);
        if (magic_mime_str != NULL) {
            doc->mime = mime_get_mime_by_string(ScanCtx.mime_table, magic_mime_str);

            LOG_DEBUGF(job->filepath, "libmagic: %s", magic_mime_str);

            if (doc->mime == 0) {
                LOG_WARNINGF(job->filepath, "Couldn't find mime %s", magic_mime_str);
            }
        }

        if (job->vfile.reset != NULL) {
            job->vfile.reset(&job->vfile);
        }

        magic_close(magic);
    }

    int mmime = MAJOR_MIME(doc->mime);

    if (!(SHOULD_PARSE(doc->mime))) {

    } else if (IS_RAW(doc->mime)) {
        parse_raw(&ScanCtx.raw_ctx, &job->vfile, doc);
    } else if ((mmime == MimeVideo && doc->size >= MIN_VIDEO_SIZE) ||
               (mmime == MimeImage && doc->size >= MIN_IMAGE_SIZE) || mmime == MimeAudio) {

        parse_media(&ScanCtx.media_ctx, &job->vfile, doc, mime_get_mime_text(doc->mime));

    } else if (IS_PDF(doc->mime)) {
        parse_ebook(&ScanCtx.ebook_ctx, &job->vfile, mime_get_mime_text(doc->mime), doc);

    } else if (mmime == MimeText && ScanCtx.text_ctx.content_size > 0) {
        if (IS_MARKUP(doc->mime)) {
            parse_markup(&ScanCtx.text_ctx, &job->vfile, doc);
        } else {
            parse_text(&ScanCtx.text_ctx, &job->vfile, doc);
        }

    } else if (IS_FONT(doc->mime)) {
        parse_font(&ScanCtx.font_ctx, &job->vfile, doc);

    } else if (
            ScanCtx.arc_ctx.mode != ARC_MODE_SKIP && (
                    IS_ARC(doc->mime) ||
                    (IS_ARC_FILTER(doc->mime) && should_parse_filtered_file(doc->filepath, doc->ext))
            )) {
        parse_archive(&ScanCtx.arc_ctx, &job->vfile, doc, ScanCtx.exclude, ScanCtx.exclude_extra);
    } else if ((ScanCtx.ooxml_ctx.content_size > 0 || ScanCtx.media_ctx.tn_size > 0) && IS_DOC(doc->mime)) {
        parse_ooxml(&ScanCtx.ooxml_ctx, &job->vfile, doc);
    } else if (is_cbr(&ScanCtx.comic_ctx, doc->mime) || is_cbz(&ScanCtx.comic_ctx, doc->mime)) {
        parse_comic(&ScanCtx.comic_ctx, &job->vfile, doc);
    } else if (IS_MOBI(doc->mime)) {
        parse_mobi(&ScanCtx.mobi_ctx, &job->vfile, doc);
    } else if (doc->mime == MIME_SIST2_SIDECAR) {
        parse_sidecar(&job->vfile, doc);
        CLOSE_FILE(job->vfile)
        free(doc);
        return;
    } else if (is_msdoc(&ScanCtx.msdoc_ctx, doc->mime)) {
        parse_msdoc(&ScanCtx.msdoc_ctx, &job->vfile, doc);
    } else if (is_json(&ScanCtx.json_ctx, doc->mime)) {
        parse_json(&ScanCtx.json_ctx, &job->vfile, doc);
    } else if (is_ndjson(&ScanCtx.json_ctx, doc->mime)) {
        parse_ndjson(&ScanCtx.json_ctx, &job->vfile, doc);
    }

    abort:

    //Parent meta
    if (job->parent[0] != '\0') {
        meta_line_t *meta_parent = malloc(sizeof(meta_line_t) + SIST_INDEX_ID_LEN);
        meta_parent->key = MetaParent;
        strcpy(meta_parent->str_val, job->parent);
        APPEND_META((doc), meta_parent)
    }

    CLOSE_FILE(job->vfile)

    if (job->vfile.has_checksum) {
        char sha1_digest_str[SHA1_STR_LENGTH];
        buf2hex((unsigned char *) job->vfile.sha1_digest, SHA1_DIGEST_LENGTH, (char *) sha1_digest_str);
        APPEND_STR_META(doc, MetaChecksum, (const char *) sha1_digest_str);
    }

    write_document(doc);
}

void cleanup_parse() {
    // noop
}
