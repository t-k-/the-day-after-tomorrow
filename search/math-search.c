#include <stdio.h>
#include <stdlib.h>

#include "timer/timer.h"
#include "tex-parser/vt100-color.h"
#include "mem-index/mem-posting.h"

#include "config.h"
#include "postmerge.h"
#include "search.h"
#include "search-utils.h"
#include "math-expr-search.h"
#include "math-search.h"

#pragma pack(push, 1)
typedef struct {
	/* consistent variables */
	struct postmerge   *top_pm;
	enum query_kw_type *kw_type;

	/* writing posting list */
	struct mem_posting *wr_mem_po;
	uint32_t            last_visits;

	/* statical variables */
	uint32_t            n_mem_po;
	float               mem_cost;

	/* document math score item buffer */
	math_score_posting_item_t last;
	position_t reserve[MAX_HIGHLIGHT_OCCURS];

	/* index ptr for debug purpose */
	struct indices *indices;

	/* timer related */
	struct timer timer;
	long first_timecost;
	long max_timecost;

} math_score_combine_args_t;
#pragma pack(pop)

static void
msca_push_pos(math_score_combine_args_t *msca, position_t pos)
{
	uint32_t *i = &msca->last.n_match;
	if (*i < MAX_HIGHLIGHT_OCCURS) {
		msca->last.pos_arr[*i] = pos;
		(*i) ++;
	}
}

static void
msca_set(math_score_combine_args_t *msca, doc_id_t docID)
{
	msca->last.docID   = docID;
	msca->last.score   = 0;
	msca->last.n_match = 0;
}

/* debug print function */
static void print_math_score_posting(struct mem_posting*);

static void
write_math_score_posting(math_score_combine_args_t *msca)
{
	size_t wr_sz = sizeof(math_score_posting_item_t);
	math_score_posting_item_t *mip = &msca->last;

	if (mip->score > 0) {
		wr_sz += mip->n_match * sizeof(position_t);
		mem_posting_write(msca->wr_mem_po, mip, wr_sz);
	}
}

static void
add_math_score_posting(math_score_combine_args_t *msca)
{
	struct postmerge_callbks *pm_calls;

	/* flush write */
	write_math_score_posting(msca);
	mem_posting_write_complete(msca->wr_mem_po);

#ifdef DEBUG_MATH_SCORE_POSTING
	printf("\n");
	printf("adding math-score posting list:\n");
	print_math_score_posting(msca->wr_mem_po);
	printf("\n");
#endif

	/* record memory cost increase */
	msca->mem_cost += (float)msca->wr_mem_po->tot_sz;

	/* add this posting list to top level postmerge */
	pm_calls = get_memory_postmerge_callbks();
	postmerge_posts_add(msca->top_pm, msca->wr_mem_po,
	                    pm_calls, msca->kw_type);

#ifdef VERBOSE_SEARCH
	{
		long last_timecost;
		last_timecost = timer_last_msec(&msca->timer);

		/* record first-adding math posting time cost */
		if (msca->first_timecost == -1)
			msca->first_timecost = last_timecost;

		/* record max time of adding a math posting */
		if (last_timecost > msca->max_timecost)
			msca->max_timecost = last_timecost;
	}
#endif
}

static void
math_posting_on_merge(uint64_t cur_min, struct postmerge* pm,
                      void* extra_args)
{
	struct math_expr_score_res res;
	P_CAST(mesa, struct math_extra_score_arg, extra_args);
	P_CAST(msca, math_score_combine_args_t, mesa->expr_srch_arg);

	/* calculate expression similarity on merge */
	res = math_expr_score_on_merge(pm, mesa->dir_merge_level,
	                               mesa->n_qry_lr_paths);

	/* math expression with zero score is filtered. */
	if (res.score == 0)
		return;

	/* otherwise, we need to add this expression */

	/* check if we are on a new directory */
	if (msca->wr_mem_po == NULL ||
	    mesa->n_dir_visits != msca->last_visits) {
		/* create a new posting list? */

		/*
		 * keep checking if total posting lists to be added is exceeding
		 * the limit. At the mean time restrict the max number of math-
		 * postings to be added for each math expression.
		 */
		if (msca->top_pm->n_postings + 1 >= MAX_MERGE_POSTINGS ||
		    msca->n_mem_po + 1 >= MAX_POSTINGS_PER_MATH) {
			/* stop traversing the next directory */
			mesa->stop_dir_search = 1;
			return;

		} else {
			/* yes, we need to create a new posting list */

			/* swap old one */
			if (msca->wr_mem_po)
				add_math_score_posting(msca);

			/* switch to a new posting list */
			msca_set(msca, 0);
			msca->wr_mem_po = mem_posting_create(
				DEFAULT_SKIPPY_SPANS,
				math_score_posting_plain_calls()
			);
			msca->n_mem_po ++;

			/* update last_visits */
			msca->last_visits = mesa->n_dir_visits;
		}
	}

#ifdef DEBUG_PRINT_TARGET_DOC_MATH_SCORE
	if (res.doc_id == 2550 || res.doc_id == 7055) {
		printf("doc expression score: %u \n", res.score);
		print_math_expr_at(msca->indices, res.doc_id, res.exp_id);
		printf("\n");
	}

	if (res.doc_id != msca->last.docID) {
		doc_id_t _last_ID = msca->last.docID;
		if (_last_ID == 2550 || _last_ID == 7055) {
			printf(C_BLUE "final doc#%u score: %u\n\n" C_RST,
			       _last_ID, msca->last.score);
		}
	}
#endif

#ifdef DEBUG_MATH_SEARCH
	printf("\n");
	printf("directory visited: %u\n", mesa->n_dir_visits);
	printf("visting depth: %u\n", mesa->dir_merge_level);
#endif

	/*
	 * write last posting list item when this
	 * expression is a new document ID.
	 */
	if (res.doc_id != msca->last.docID) {
		write_math_score_posting(msca);
		msca_set(msca, res.doc_id);
	}

	/* finally, update current maximum expression score
	 * inside of current evaluating document, also add
	 * expression positions of current document */
	if (res.score > msca->last.score)
		msca->last.score = res.score;

	msca_push_pos(msca, res.exp_id);
}

uint32_t
add_math_postinglist(struct postmerge *pm, struct indices *indices,
                     char *kw_utf8, enum query_kw_type *kw_type)
{
	math_score_combine_args_t msca;
	int64_t n_tot_rd_items;

	/* initialize score combine arguments */
	msca.top_pm      = pm;
	msca.kw_type     = kw_type;
	msca.wr_mem_po   = NULL;
	msca.last_visits = 0;
	msca.n_mem_po    = 0;
	msca.mem_cost    = 0.f;
	msca_set(&msca, 0);
	msca.indices     = indices;

#ifdef VERBOSE_SEARCH
	/* reset timer */
	timer_reset(&msca.timer);
	msca.first_timecost = -1;
	msca.max_timecost = 0;
#endif

	/* merge and combine math scores */
	n_tot_rd_items = math_expr_search(indices->mi, kw_utf8,
	                                  DIR_MERGE_DEPTH_FIRST,
	                                  &math_posting_on_merge,
	                                  &msca);
	if (msca.wr_mem_po)
		/* flush and final adding */
		add_math_score_posting(&msca);
	else
		/* add a NULL posting to indicate empty result */
		postmerge_posts_add(msca.top_pm, NULL, NULL, kw_type);

#ifdef VERBOSE_SEARCH
	/* report number of adding math posting lists. */
	printf("`%s' has %u math score posting(s) (%f KB).\n",
	       kw_utf8, msca.n_mem_po, msca.mem_cost / 1024.f);

	/* report number of read items */
	printf("math post-adding total read items: %ld."
	       " (negative indicates parse error)\n",
	       n_tot_rd_items);

	/* report time cost */
	printf("math post-adding first time cost: %ld msec.\n",
	       msca.first_timecost);
	printf("math post-adding max time cost: %ld msec.\n",
	       msca.max_timecost);
	printf("math post-adding average time cost: %.2f msec.\n",
	       (float)timer_tot_msec(&msca.timer) / msca.n_mem_po);
	printf("math post-adding total time cost: %ld msec.\n",
	       timer_tot_msec(&msca.timer));
#endif

	return msca.n_mem_po;
}

static void print_math_score_posting(struct mem_posting *po)
{
	math_score_posting_item_t *mip;
	if (!mem_posting_start(po)) {
		printf("(empty)");
		return;
	}

	do {
		mip = mem_posting_cur_item(po);
		printf("[docID=%u, score=%u, n_match=%u", mip->docID,
			   mip->score, mip->n_match);

		if (mip->n_match != 0) {
			uint32_t i;
			printf(": ");
			for (i = 0; i < mip->n_match; i++)
				printf("%u ", mip->pos_arr[i]);
		}

		printf("]");
	} while (mem_posting_next(po));

	mem_posting_finish(po);
}
