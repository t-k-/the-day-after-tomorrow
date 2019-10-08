#include <assert.h>

#include "dir-util/dir-util.h" /* for MAX_FILE_NAME_LEN */
#include "indices.h"

static void indices_update_stats(struct indices* indices)
{
	if (indices->ti) {
		indices->n_doc     = term_index_get_docN(indices->ti);
		indices->avgDocLen = term_index_get_avgDocLen(indices->ti);
	}

	if (indices->mi) {
		indices->n_tex    = indices->mi->stats.n_tex;
		indices->n_secttr = indices->mi->stats.N;
	}
}

int indices_open(struct indices* indices, const char* index_path,
                  enum indices_open_mode mode)
{
	/* return variable */
	int                   open_err = 0;

	/* path strings */
	const char            blob_index_url_name[] = "url";
	const char            blob_index_txt_name[] = "doc";
	char                  path[MAX_FILE_NAME_LEN];

	/* indices shorthand variables */
	term_index_t          term_index = NULL;
	math_index_t          math_index = NULL;
	blob_index_t          blob_index_url = NULL;
	blob_index_t          blob_index_txt = NULL;

	/* zero all fields */
	memset(indices, 0, sizeof(struct indices));

	/*
	 * open term index.
	 */
	sprintf(path, "%s/term", index_path);

	if (mode == INDICES_OPEN_RW)
		mkdir_p(path);

	term_index = term_index_open(path, (mode == INDICES_OPEN_RD) ?
	                             TERM_INDEX_OPEN_EXISTS:
	                             TERM_INDEX_OPEN_CREATE);
	if (NULL == term_index) {
		fprintf(stderr, "cannot create/open term index.\n");
		open_err = 1;

		goto skip;
	}

	/*
	 * open math index.
	 */
	math_index = math_index_open(index_path, (mode == INDICES_OPEN_RD) ?
	                             "r": "w");
	if (NULL == math_index) {
		fprintf(stderr, "cannot create/open math index.\n");

		open_err = 1;
		goto skip;
	}

	/*
	 * open blob index
	 */
	sprintf(path, "%s/%s", index_path, blob_index_url_name);
	blob_index_url = blob_index_open(path, (mode == INDICES_OPEN_RD) ?
	                                 BLOB_OPEN_RD : BLOB_OPEN_WR);
	if (NULL == blob_index_url) {
		fprintf(stderr, "cannot create/open URL blob index.\n");

		open_err = 1;
		goto skip;
	}

	sprintf(path, "%s/%s", index_path, blob_index_txt_name);
	blob_index_txt = blob_index_open(path, (mode == INDICES_OPEN_RD) ?
	                                 BLOB_OPEN_RD : BLOB_OPEN_WR);
	if (NULL == blob_index_txt) {
		fprintf(stderr, "cannot create/open text blob index.\n");

		open_err = 1;
		goto skip;
	}

skip:
	/* assign shorthand variables */
	indices->ti = term_index;
	indices->mi = math_index;
	indices->url_bi = blob_index_url;
	indices->txt_bi = blob_index_txt;

	/* set cache memory limits */
	indices->mi_cache_limit = DEFAULT_MATH_INDEX_CACHE_SZ;
	indices->ti_cache_limit = DEFAULT_TERM_INDEX_CACHE_SZ;

	/* set index stats */
	indices_update_stats(indices);

	return open_err;
}

void indices_close(struct indices* indices)
{
	if (indices->ti) {
		term_index_close(indices->ti);
		indices->ti = NULL;
	}

	if (indices->mi) {
		math_index_close(indices->mi);
		indices->mi = NULL;
	}

	if (indices->url_bi) {
		blob_index_close(indices->url_bi);
		indices->url_bi = NULL;
	}

	if (indices->txt_bi) {
		blob_index_close(indices->txt_bi);
		indices->txt_bi = NULL;
	}
}

int indices_cache(struct indices* indices)
{
	int res = 0;
	res |= math_index_load(indices->mi, indices->mi_cache_limit);
	indices->memo_usage += indices->mi->memo_usage;

	res |= term_index_load(indices->ti, indices->ti_cache_limit);
	indices->memo_usage += term_index_cache_memo_usage(indices->ti);

	return res;
}

void indices_print_summary(struct indices* indices)
{
	printf("[ Indices ] cached %u KB \n", indices->memo_usage);
	printf("term index: documents=%u, avg docLen=%u \n",
		indices->n_doc, indices->avgDocLen);
	printf("math index: TeXs=%u, sector trees=%u \n",
		indices->n_tex, indices->n_secttr);
}

/*
 * Below are indexer implementation.
 */
#include <ctype.h> /* for tolower() */
#include "txt-seg/lex.h" /* for g_lex_handler */

static struct indexer *g_indexer = NULL;

static void
index_blob(blob_index_t bi, doc_id_t docID, const char *str, size_t str_sz,
           bool compress)
{
	struct codec codec = {CODEC_GZ, NULL};
	size_t compressed_sz;
	void  *compressed;

	if (compress) {
		compressed_sz = codec_compress(&codec, str, str_sz, &compressed);
		blob_index_write(bi, docID, compressed, compressed_sz);
		free(compressed);
	} else {
		blob_index_write(bi, docID, str, str_sz);
	}
}

static void strip_math_tag(char *str, size_t n_bytes)
{
	size_t tag_sz = strlen("[imath]");
	uint32_t i;
	for (i = 0; tag_sz + i + 1 < n_bytes - tag_sz; i++) {
		str[i] = str[tag_sz + i];
	}

	str[i] = '\0';
}

static void eng_to_lower_case(char *str, size_t n)
{
	size_t i;
	for(i = 0; i < n; i++)
		str[i] = tolower(str[i]);
}

static struct tex_parse_ret
index_tex(math_index_t mi, char *tex, doc_id_t docID, uint32_t expID)
{
	struct tex_parse_ret parse_ret;
#ifdef DEBUG_INDEXER
	printf("[parse tex] `%s'\n", tex);
#endif
	parse_ret = tex_parse(tex, 0, false, false);

	if (parse_ret.code != PARSER_RETCODE_ERR) {
		/* add TeX into inverted index */
		math_index_add(mi, docID, expID, parse_ret.subpaths);
		subpaths_release(&parse_ret.subpaths);
	}

	return parse_ret;
}

static int indexer_handle_slice(struct lex_slice *slice)
{
	struct indices *indices = g_indexer->indices;
	size_t str_sz = strlen(slice->mb_str);
	struct tex_parse_ret tex_parse_ret;

#ifdef DEBUG_INDEXER
	printf("input slice: [%s] <%u, %lu>\n", slice->mb_str,
		slice->offset, str_sz);
#endif

	switch (slice->type) {
	case LEX_SLICE_TYPE_MATH_SEG:
		/* term_index_doc_add() is invoked here to make position numbers
		 * synchronous in both math-index and term-index. */
		term_index_doc_add(indices->ti, "math_exp");

		/* extract tex from math tag */
		strip_math_tag(slice->mb_str, str_sz);

		/* index TeX */
		tex_parse_ret = index_tex(indices->mi, slice->mb_str,
		                          indices->n_doc + 1, g_indexer->cur_position);
		/* increments */
		g_indexer->cur_position ++;
		g_indexer->n_parse_tex ++;

		if (tex_parse_ret.code == PARSER_RETCODE_ERR) {
			/* on parser error */
			g_indexer->n_parse_err++;

			parser_exception_callbk callbk = g_indexer->on_parser_exception;
			if (callbk) /* invoke parser error callback */
				callbk(g_indexer, slice->mb_str, tex_parse_ret.msg);
			return 1;
		}

		break;

	case LEX_SLICE_TYPE_ENG_SEG:
		/* turn all indexing words to lower case for recall */
		eng_to_lower_case(slice->mb_str, str_sz);

		/* add term into inverted-index */
		term_index_doc_add(indices->ti, slice->mb_str);

		/* increments */
		g_indexer->cur_position ++;
		break;

	case LEX_SLICE_TYPE_MIX_SEG: /* Non-English segmentation */
		assert(0); /* not implemented */
		break;

	default:
		assert(0);
	}

	return 0;
}

struct indexer
*indexer_alloc(struct indices *indices, text_lexer lexer,
               parser_exception_callbk on_exception)
{
	struct indexer *indexer = calloc(1, sizeof *indexer);
	indexer->indices = indices;
	indexer->lexer = lexer;
	indexer->on_parser_exception = on_exception;

	/* register global lexer handler */
	g_lex_handler = &indexer_handle_slice;
	return indexer;
}

void indexer_free(struct indexer *indexer)
{
	free(indexer);
}

int indexer_write_all_fields(struct indexer *indexer)
{
	struct indices *indices = indexer->indices;

	/* index URL field */
	index_blob(indices->url_bi, indices->n_doc + 1,
		indexer->url_field, strlen(indexer->url_field), 0);

	/* index TEXT field */
	size_t txt_sz = strlen(indexer->txt_field);
	FILE  *fh_txt = fmemopen((void *)indexer->txt_field, txt_sz, "r");

	assert(fh_txt != NULL);

	/* prepare indexing */
	term_index_doc_begin(indices->ti);

	/* invoke lexer */
	g_indexer = indexer;
	int ret = indexer->lexer(fh_txt);
	fclose(fh_txt);

	/* index TEXT blob */
	index_blob(indices->txt_bi, indices->n_doc + 1,
		indexer->txt_field, txt_sz, 1);

	/* finishing index and update docID */
	doc_id_t docID = term_index_doc_end(indices->ti);
	assert(docID == indices->n_doc + 1);
	indices->n_doc = docID;

	/* reset lexer position */
	indexer->cur_position = 0;

	/* update indices stats */
	indices_update_stats(indices);
	return ret;
}
