#include "subpath-set.h"

struct cmp_subpath_nodes_arg {
	struct list_it    path_node2;
	struct list_node *path_node2_end;
	int res;
	int skip_the_first;
	int max_cmp_nodes;
	int cnt_cmp_nodes;
};

static LIST_IT_CALLBK(cmp_subpath_nodes)
{
	LIST_OBJ(struct subpath_node, n1, ln);
	P_CAST(arg, struct cmp_subpath_nodes_arg, pa_extra);
	struct subpath_node *n2 = MEMBER_2_STRUCT(arg->path_node2.now,
	                                          struct subpath_node, ln);
	if (n2 == NULL) {
		arg->res = 1;
		return LIST_RET_BREAK;
	}

	if (n1->token_id == n2->token_id || arg->skip_the_first) {
		arg->skip_the_first = 0;

		/* reaching max compare? */
		if (arg->max_cmp_nodes != 0) {
			arg->cnt_cmp_nodes ++;
			if (arg->cnt_cmp_nodes == arg->max_cmp_nodes) {
				arg->res = 0;
				return LIST_RET_BREAK;
			}
		}

		/* for tokens compare */
		if (arg->path_node2.now == arg->path_node2_end) {
			if (pa_now->now == pa_head->last)
				arg->res = 0;
			else
				arg->res = 2;

			return LIST_RET_BREAK;
		} else {
			if (pa_now->now == pa_head->last) {
				arg->res = 3;
				return LIST_RET_BREAK;
			} else {
				arg->path_node2 = list_get_it(arg->path_node2.now->next);
				return LIST_RET_CONTINUE;
			}
		}
	} else {
		arg->res = 4;
		return LIST_RET_BREAK;
	}
}

static int
cmp_subpaths(struct subpath *sp1, struct subpath *sp2, int prefix_len)
{
	struct cmp_subpath_nodes_arg arg;
	arg.path_node2 = sp2->path_nodes;
	arg.path_node2_end = sp2->path_nodes.last;
	arg.res = 0;
	arg.skip_the_first = 0;
	arg.max_cmp_nodes = prefix_len;
	arg.cnt_cmp_nodes = 0;

	if (sp1->type != sp2->type) {
		arg.res = 5;
	} else {
		if (sp1->type == SUBPATH_TYPE_GENERNODE ||
		    sp1->type == SUBPATH_TYPE_WILDCARD)
			arg.skip_the_first = 1;

		list_foreach(&sp1->path_nodes, &cmp_subpath_nodes, &arg);
	}

#ifdef DEBUG_SUBPATH_SET
	printf("two compared paths ");
	switch (arg.res) {
	case 0:
		printf("are the same.");
		break;
	case 1:
		printf("are different. (the other is empty)");
		break;
	case 2:
		printf("are different. (the other is shorter)");
		break;
	case 3:
		printf("are different. (the other is longer)");
		break;
	case 4:
		printf("are different. (node tokens do not match)");
		break;
	case 5:
		printf("are different. (the other is not the same type)");
		break;
	default:
		printf("unexpected res number.\n");
	}
	printf("\n");
#endif

	return arg.res;
}

struct add_subpath_args {
	linkli_t *set;
	uint32_t  prefix_len;
	int       added;
};

struct prefix_path_root_args {
	uint32_t height;
	uint32_t cnt;
	struct subpath_node *node;
};

static LIST_IT_CALLBK(_prefix_path_root)
{
	LIST_OBJ(struct subpath_node, sp_nd, ln);
	P_CAST(args, struct prefix_path_root_args, pa_extra);

	args->cnt ++;
	args->node = sp_nd;

	if (args->height == args->cnt) {
		return LIST_RET_BREAK;
	} else {
		LIST_GO_OVER;
	}
}

static struct subpath_node*
prefix_path_root(struct subpath *sp, uint32_t prefix_len)
{
	struct prefix_path_root_args args = {prefix_len, 0, NULL};
	list_foreach(&sp->path_nodes, &_prefix_path_root, &args);

	return args.node;
}

static struct subpath_ele *new_ele(uint32_t prefix_len)
{
	struct subpath_ele *newele;
	newele = malloc(sizeof(struct subpath_ele));

	li_node_init(newele->ln);

	newele->dup_cnt = 0;
	newele->n_sects = 0;
	newele->prefix_len = prefix_len;

	return newele;
}

static void ele_add_dup(struct subpath_ele *ele, struct subpath *sp)
{
	struct subpath_node *root = prefix_path_root(sp, ele->prefix_len);
	uint32_t n_dup = ele->dup_cnt;
	/* avoid dup array overflow, possible in wildcards paths. */
	if (n_dup < MAX_MATH_PATHS) {
		ele->dup[n_dup]  = sp;
		ele->rid[n_dup]  = root->node_id;
		ele->rtok[n_dup] = root->token_id;
	}
}

static LIST_IT_CALLBK(add_into_set)
{
	LIST_OBJ(struct subpath, sp, ln);
	P_CAST(args, struct add_subpath_args, pa_extra);

	if (args->prefix_len > sp->n_nodes) {
		LIST_GO_OVER;
	}

	if (*args->set == NULL) {
		/* adding the first element */
		;
	} else {
		linkli_t set = *args->set;
		foreach (iter, li, set) {
			struct subpath_ele *ele = li_entry(ele, iter->cur, ln);
			if (ele->prefix_len == args->prefix_len &&
				0 == cmp_subpaths(sp, ele->dup[0], args->prefix_len)) {
				/* this prefix path belongs to an element */
				ele->dup_cnt ++;
				ele_add_dup(ele, sp);
				args->added ++;
				/* `goto' in FOREACH requires a manual li_iter_free() */
				li_iter_free(iter);
				goto next;
			}
		}
	}

	/* adding a new unique element */
	struct subpath_ele *newele;
	newele = new_ele(args->prefix_len);
	ele_add_dup(newele, sp);
	li_append(args->set, &newele->ln);
	args->added ++;

next:
	LIST_GO_OVER;
}

linkli_t subpath_set(struct subpaths subpaths, enum subpath_set_opt opt)
{
	linkli_t set = NULL;
	struct add_subpath_args args = {&set, 0, 0};

	for (args.prefix_len = 2;; args.prefix_len ++) {
		args.added = 0;	
		list_foreach(&subpaths.li, &add_into_set, &args);

		printf("%d paths added at prefix length = %u... \n",
			args.added, args.prefix_len);

		if (!args.added) break;
	}

	return set;
}
