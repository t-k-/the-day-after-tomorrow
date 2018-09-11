#pragma once
#include "indices/indices.h"
#include "math-prefix-qry.h"

struct math_qry_struct {
	struct math_prefix_qry pq;
	struct subpaths subpaths;
	list subpath_set;
	int n_uniq_paths;
	int n_qry_nodes;
};

int math_qry_prepare(struct indices*, char*, struct math_qry_struct*);

void math_qry_free(struct math_qry_struct*);