/*
** Copyright (C) 2014 Jerome Kelleher <jerome.kelleher@well.ox.ac.uk>
**
** This file is part of msprime.
**
** msprime is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
**
** msprime is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with msprime.  If not, see <http://www.gnu.org/licenses/>.
*/
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <avl.h>
#include <gsl/gsl_rng.h>
#include <gsl/gsl_math.h>
#include <gsl/gsl_randist.h>
#include <gsl/gsl_statistics_int.h>

#include "util.h"
#include "fenwick.h"
#include "msprime.h"

static int
cmp_individual(const void *a, const void *b) {
    const segment_t *ia = (const segment_t *) a;
    const segment_t *ib = (const segment_t *) b;
    return (ia->index > ib->index) - (ia->index < ib->index);
}

static int
cmp_node_mapping(const void *a, const void *b) {
    const node_mapping_t *ia = (const node_mapping_t *) a;
    const node_mapping_t *ib = (const node_mapping_t *) b;
    return (ia->left > ib->left) - (ia->left < ib->left);
}

static inline unsigned int
num_links(segment_t *u)
{
    return u->right - u->left;
}

static double
constant_population_model_get_size(population_model_t *self, double t)
{
    return self->param;
}

static double
constant_population_model_get_waiting_time(population_model_t *self,
        double lambda_coancestry, double t, gsl_rng* rng)
{
    return self->param * gsl_ran_exponential(rng, 1.0 / lambda_coancestry);
}

static double
exponential_population_model_get_size(population_model_t *self, double t)
{
    double alpha = self->param;
    return exp(-alpha * (t - self->start_time));
}

static double
exponential_population_model_get_waiting_time(population_model_t *self,
        double lambda_coancestry, double t, gsl_rng* rng)
{
    double ret = DBL_MAX;
    double alpha = self->param;
    double dt = t - self->start_time;
    double z = alpha * self->initial_size * exp(-alpha * dt);
    z = 1 + z * gsl_ran_exponential(rng, 1.0 / lambda_coancestry);
    /* if z is <= 0 no coancestry can occur */
    if (z > 0) {
        ret = log(z) / alpha;
    }
    return ret;
}

static void
msp_add_population_model(msp_t *self, population_model_t *model)
{
    population_model_t *m = self->population_models;
    if (m == NULL) {
        self->population_models = model;
    } else {
        while (m->next != NULL) {
            m = m->next;
        }
        m->next = model;
    }
    model->next = NULL;
}

void
msp_add_constant_population_model(msp_t *self, double time, double size)
{
    population_model_t *model = xmalloc(sizeof(population_model_t));
    model->start_time = time;
    model->param = size;
    model->type = POP_MODEL_CONSTANT;
    model->get_size = constant_population_model_get_size;
    model->get_waiting_time = constant_population_model_get_waiting_time;
    msp_add_population_model(self, model);
}

void
msp_add_exponential_population_model(msp_t *self, double time, double alpha)
{
    population_model_t *model = xmalloc(sizeof(population_model_t));
    model->start_time = time;
    model->param = alpha;
    model->type = POP_MODEL_EXPONENTIAL;
    model->get_size = exponential_population_model_get_size;
    model->get_waiting_time = exponential_population_model_get_waiting_time;
    msp_add_population_model(self, model);
}

void
msp_alloc(msp_t *self)
{
    int n, j;
    avl_node_t *node;
    segment_t *seg;

    self->rng = gsl_rng_alloc(gsl_rng_default);
    gsl_rng_set(self->rng, self->random_seed);
    /* Allocate the memory heaps */
    n = self->max_avl_nodes;
    self->avl_node_mem = xmalloc(n * sizeof(avl_node_t));
    self->avl_node_heap = xmalloc(n * sizeof(avl_node_t *));
    for (j = 0; j < n; j++) {
        node = &self->avl_node_mem[j];
        self->avl_node_heap[j] = node;
    }
    self->avl_node_heap_top = (int) n - 1;
    n = self->max_segments;
    self->segment_mem = xmalloc(n * sizeof(segment_t));
    self->segment_heap = xmalloc(n * sizeof(segment_t *));
    for (j = 0; j < n; j++) {
        seg = &self->segment_mem[j];
        self->segment_heap[j] = seg;
        seg->index = j + 1;
    }
    self->segment_heap_top = (int) n - 1;
    self->links = xmalloc(sizeof(fenwick_t));
    self->links->max_index = n;
    fenwick_alloc(self->links);
    n = self->max_trees;
    self->node_mapping_mem = xmalloc(n * sizeof(node_mapping_t));
    self->next_node_mapping = 0;
    n = self->max_coalescence_records;
    self->coalescence_records = xmalloc(n * sizeof(coalescence_record_t));
    self->next_coalescence_record = 0;
    /* set up the AVL trees */
    self->population = xmalloc(sizeof(avl_tree_t));
    avl_init_tree(self->population, cmp_individual, NULL);
    self->breakpoints = xmalloc(sizeof(avl_tree_t));
    avl_init_tree(self->breakpoints, cmp_node_mapping, NULL);
    self->replicate_number = 0;
}

void
msp_free(msp_t *self)
{
    population_model_t *u, *v;

    u = self->population_models;
    while (u != NULL) {
        v = u->next;
        free(u);
        u = v;
    }
    gsl_rng_free(self->rng);
    fenwick_free(self->links);
    free(self->links);
    free(self->population);
    free(self->breakpoints);
    free(self->node_mapping_mem);
    free(self->avl_node_mem);
    free(self->avl_node_heap);
    free(self->segment_mem);
    free(self->segment_heap);
    free(self->coalescence_records);
    free(self->coalescence_record_file_pattern);
}

static avl_node_t *
msp_alloc_avl_node(msp_t *self)
{
    avl_node_t *node = self->avl_node_heap[self->avl_node_heap_top];
    self->avl_node_heap_top--;
    if (self->avl_node_heap_top < 0) {
        fatal_error("out of avl nodes");
    }
    return node;
}

static void
msp_free_avl_node(msp_t *self, avl_node_t *node)
{
    self->avl_node_heap_top++;
    assert(self->avl_node_heap_top < (int) self->max_avl_nodes);
    self->avl_node_heap[self->avl_node_heap_top] = node;
}

static segment_t *
msp_alloc_segment(msp_t *self, int left, int right, int value, segment_t *prev,
        segment_t *next)
{
    segment_t *seg = self->segment_heap[self->segment_heap_top];
    unsigned int num_segments;
    self->segment_heap_top--;
    if (self->segment_heap_top < 0) {
        fatal_error("out of segments");
    }
    seg->prev = prev;
    seg->next = next;
    seg->left = left;
    seg->right = right;
    seg->value = value;
    num_segments = self->max_segments - self->segment_heap_top - 1;
    if (num_segments > self->max_used_segments) {
        self->max_used_segments = num_segments;
    }
    return seg;
}

/*
 * Returns the segment with the specified index.
 */
static segment_t *
msp_get_segment(msp_t *self, unsigned int index)
{
    segment_t *u = &self->segment_mem[index - 1];
    assert(u->index == index);
    return u;
}

static void
msp_free_segment(msp_t *self, segment_t *node)
{
    self->segment_heap_top++;
    assert(self->segment_heap_top < (int) self->max_segments);
    self->segment_heap[self->segment_heap_top] = node;
    fenwick_set_value(self->links, node->index, 0);
}


static void
msp_flush_coalescence_records(msp_t *self)
{
    size_t ret;
    if (self->coalescence_record_file != NULL) {
        ret = fwrite(self->coalescence_records, sizeof(coalescence_record_t),
                self->next_coalescence_record, self->coalescence_record_file);
        if (ret != self->next_coalescence_record) {
            fatal_error("error writing to %s: %s",
                    self->coalescence_record_filename, strerror(errno));
        }
    }
    self->next_coalescence_record = 0;
}

/*
 * Write a simple header including a magic number to check and
 * the sample size so we can reconstruct the trees.
 */
static void
msp_write_coalescence_record_file_header(msp_t *self)
{
    FILE *f = self->coalescence_record_file;
    unsigned int header[2];
    size_t ret;

    assert(f != NULL);
    header[0] = TREEFILE_MAGIC;
    header[1] = self->sample_size;
    ret = fwrite(header, sizeof(header), 1, self->coalescence_record_file);
    if (ret != 1) {
        fatal_error("error writing to %s: %s",
                self->coalescence_record_filename, strerror(errno));
    }
}

/*
 * Write the file footer, which is a zero sentinel to signal
 * the end of the stream of coalescence records.
 */
static void
msp_write_coalescence_record_file_footer(msp_t *self)
{
    size_t ret;
    coalescence_record_t sentinel;
    FILE *f = self->coalescence_record_file;

    assert(f != NULL);
    memset(&sentinel, 0, sizeof(sentinel));
    ret = fwrite(&sentinel, sizeof(coalescence_record_t), 1, f);
    if (ret != 1) {
        fatal_error("error writing to %s: %s",
                self->coalescence_record_filename, strerror(errno));
    }
}

static void
msp_open_coalescence_record_file(msp_t *self)
{
    size_t len;
    FILE *f;
    self->coalescence_record_file = NULL;
    if (strlen(self->coalescence_record_file_pattern) > 0) {
        len = snprintf(NULL, 0, self->coalescence_record_file_pattern,
                self->replicate_number);
        self->coalescence_record_filename = xmalloc(len + 1);
        snprintf(self->coalescence_record_filename, len + 1,
                self->coalescence_record_file_pattern, self->replicate_number);
        f = fopen(self->coalescence_record_filename, "w");
        if (f == NULL) {
            fatal_error("cannot open %s: %s",
                    self->coalescence_record_filename, strerror(errno));
        }
        self->coalescence_record_file = f;
        msp_write_coalescence_record_file_header(self);
    }
}

static void
msp_close_coalescence_record_file(msp_t *self)
{
    FILE *f = self->coalescence_record_file;
    assert(f != NULL);
    if (fclose(f) != 0) {
        fatal_error("cannot close %s: %s",
                self->coalescence_record_filename, strerror(errno));
    }
    free(self->coalescence_record_filename);
}



/*
 * Reset the state of the simulator so that it is ready to compute another
 * replicate.
 */
void
msp_reset(msp_t *self)
{
    avl_node_t *node;
    /* Free the breakpoints */
    for (node = self->breakpoints->head; node != NULL; node = node->next) {
        msp_free_avl_node(self, node);
    }
    avl_clear_tree(self->breakpoints);
    self->next_coalescence_record = 0;
    self->replicate_number++;
    /* check for leaks */
    assert(self->avl_node_heap_top == self->max_avl_nodes - 1);
    assert(self->segment_heap_top == self->max_segments - 1);

}

static inline void
msp_insert_individual(msp_t *self, segment_t *u)
{
    avl_node_t *node;
    assert(u != NULL);
    node = msp_alloc_avl_node(self);
    avl_init_node(node, u);
    node = avl_insert_node(self->population, node);
    assert(node != NULL);
}

/*
 * Returns the tree for the specified locus
 */
static void
msp_get_tree(msp_t *self, unsigned int locus, int *pi, float *tau)
{
    unsigned int j;
    coalescence_record_t *cr;
    memset(pi, 0, 2 * self->sample_size * sizeof(int));
    memset(tau, 0, 2 * self->sample_size * sizeof(float));
    for (j = 0; j < self->next_coalescence_record; j++) {
        cr = &self->coalescence_records[j];
        if (cr->left <= locus && locus <= cr->right) {
            pi[cr->children[0]] = cr->parent;
            pi[cr->children[1]] = cr->parent;
            tau[cr->parent] = cr->time;
        }
    }
}

static void
msp_print_segment_chain(msp_t *self, segment_t *head)
{
    int j;
    segment_t *s = head;
    j = 1;
    while (s != NULL) {
        while (j < s->left) {
            printf("%4c", '-');
            j++;
        }
        while (j <= s->right) {
            printf("%4d", s->value);
            j++;
        }
        s = s->next;
    }
    while (j <= self->num_loci) {
        printf("%4c", '-');
        j++;
    }
    printf("\n");
}

/*
 * Prints out the tree that intersects with the specified locus.
 */
static void
msp_print_tree(msp_t *self, unsigned int l)
{
    unsigned int j;
    int *pi = xmalloc(2 * self->sample_size * sizeof(int));
    float *tau = xmalloc(2 * self->sample_size * sizeof(float));
    msp_get_tree(self, l, pi, tau);
    printf("\t%4d:", l);
    for (j = 0; j < 2 * self->sample_size; j++) {
        printf("%3d ", pi[j]);
    }
    printf("|");
    for (j = 0; j < 2 * self->sample_size; j++) {
        printf("%.3f ", tau[j]);
    }
    printf("\n");
    free(pi);
    free(tau);
}

static void
msp_verify(msp_t *self)
{
    int j;
    long long s, ss, total_links;
    unsigned int total_segments = 0;
    unsigned int total_avl_nodes = 0;
    int *pi = xmalloc(2 * self->sample_size * sizeof(int));
    float *tau = xmalloc(2 * self->sample_size * sizeof(float));
    avl_node_t *node;
    segment_t *u;

    total_links = 0;
    node = self->population->head;
    while (node != NULL) {
        u = (segment_t *) node->item;
        assert(u->prev == NULL);
        while (u != NULL) {
            total_segments++;
            assert(u->left <= u->right);
            for (j = u->left; j <= u->right; j++) {
                msp_get_tree(self, j, pi, tau);
                assert(pi[u->value] == 0);
            }
            if (u->prev != NULL) {
                s = u->right - u->prev->right;
            } else {
                s = u->right - u->left;
            }
            ss = fenwick_get_value(self->links, u->index);
            total_links += ss;
            assert(s == ss);
            u = u->next;
        }
        node = node->next;
    }
    assert(total_links == fenwick_get_total(self->links));
    assert(total_segments == self->max_segments - self->segment_heap_top - 1);
    total_avl_nodes = avl_count(self->population)
            + avl_count(self->breakpoints);
    assert(total_avl_nodes == self->max_avl_nodes -
            self->avl_node_heap_top - 1);
    free(pi);
    free(tau);
}

void
msp_print_stats(msp_t *self)
{
    int num_trees = avl_count(self->breakpoints) - 1;
    printf("%f\t%d\t%u\t%u\t%u\t%u\t%u\n", self->time, num_trees,
            self->num_re_events, self->num_ca_events,
            self->num_coalescence_records, self->max_used_segments,
            self->max_population_size);
}

void
msp_print_state(msp_t *self)
{
    avl_node_t *node;
    node_mapping_t *nm;
    coalescence_record_t *cr;
    segment_t *u;
    long long v;
    unsigned int j;

    printf("n = %d\n", self->sample_size);
    printf("m = %d\n", self->num_loci);
    printf("random seed = %ld\n", self->random_seed);
    printf("num_links = %lld\n", fenwick_get_total(self->links));
    printf("population = %d\n", avl_count(self->population));
    node = self->population->head;
    while (node != NULL) {
        u = (segment_t *) node->item;
        printf("\t");
        msp_print_segment_chain(self, u);
        node = node->next;
    }
    printf("trees = %d\n", avl_count(self->breakpoints));
    /* Skip the last tree as it's not real */
    for (node = self->breakpoints->head; node->next != NULL; node = node->next) {
        nm = (node_mapping_t *) node->item;
        msp_print_tree(self, nm->left);
    }
    printf("Fenwick tree\n");
    for (j = 1; j <= self->max_segments; j++) {
        u = msp_get_segment(self, j);
        v = fenwick_get_value(self->links, j);
        if (v != 0) {
            printf("\t%lld\tl=%d r=%d v=%d prev=%p next=%p\n", v, u->left,
                    u->right, u->value, u->prev, u->next);
        }
    }
    printf("Breakpoints = %d\n", avl_count(self->breakpoints));
    for (node = self->breakpoints->head; node != NULL; node = node->next) {
        nm = (node_mapping_t *) node->item;
        printf("\t%d -> %d\n", nm->left, nm->value);
    }
    printf("Coalescence records = %d\n", self->next_coalescence_record);
    for (j = 0; j < self->next_coalescence_record; j++) {
        cr = &self->coalescence_records[j];
        printf("\t(%d, %d) -- (%d, %d)->%d @ %f\n", cr->left, cr->right,
                cr->children[0], cr->children[1], cr->parent, cr->time);
    }
    msp_verify(self);
}

/*
 * Inserts a new breakpoint at the specified locus left, mapping to the
 * specified tree node v.
 */
static void
msp_insert_breakpoint(msp_t *self, unsigned int left, int v)
{
    avl_node_t *node = msp_alloc_avl_node(self);
    node_mapping_t *m;

    if (self->next_node_mapping == self->max_trees) {
        fatal_error("out of node_mappings");
    }
    m = &self->node_mapping_mem[self->next_node_mapping];
    self->next_node_mapping++;
    m->left = left;
    m->value = v;
    avl_init_node(node, m);
    node = avl_insert_node(self->breakpoints, node);
    assert(node != NULL);
}

/*
 * Inserts a new breakpoint at the specified locus, and copies it's
 * node mapping from the containing breakpoint.
 */
static void
msp_copy_breakpoint(msp_t *self, unsigned int k)
{
    node_mapping_t search, *nm;
    avl_node_t *node;
    search.left = k;
    avl_search_closest(self->breakpoints, &search, &node);
    assert(node != NULL);
    nm = (node_mapping_t *) node->item;
    if (nm->left > k) {
        node = node->prev;
        assert(node != NULL);
        nm = (node_mapping_t *) node->item;
    }
    msp_insert_breakpoint(self, k, nm->value);
}

static void
msp_record_coalescence(msp_t *self, unsigned int left, unsigned int right,
        int child1, int child2, int parent)
{
    coalescence_record_t *r = NULL;
    if (self->next_coalescence_record == self->max_coalescence_records) {
        msp_flush_coalescence_records(self);
    }
    r = &self->coalescence_records[self->next_coalescence_record];
    self->next_coalescence_record++;
    r->left = left;
    r->right = right;
    r->children[0] = child1;
    r->children[1] = child2;
    r->parent = parent;
    r->time = self->time;
    self->num_coalescence_records++;
}

static void
msp_recombination_event(msp_t *self)
{
    long long l, t, gap, k;
    unsigned int j;
    node_mapping_t search;
    segment_t *x, *y, *z;
    long long num_links = fenwick_get_total(self->links);

    self->num_re_events++;
    /* We can't use the GSL integer generator here as the range is too large */
    l = 1 + (long long) (gsl_rng_uniform(self->rng) * num_links);
    assert(l > 0 && l <= num_links);
    j = fenwick_find(self->links, l);
    t = fenwick_get_cumulative_sum(self->links, j);
    gap = t - l;
    y = msp_get_segment(self, j);
    x = y->prev;
    k = y->right - gap - 1;
    if (y->left <= k) {
        z = msp_alloc_segment(self, k + 1, y->right, y->value, NULL, y->next);
        fenwick_increment(self->links, z->index, z->right - k - 1);
        if (y->next != NULL) {
            y->next->prev = z;
        }
        y->next = NULL;
        y->right = k;
        fenwick_increment(self->links, y->index, k - z->right);
        search.left = k + 1;
        if (avl_search(self->breakpoints, &search) == NULL) {
            msp_copy_breakpoint(self, k + 1);
        }
    } else {
        x->next = NULL;
        y->prev = NULL;
        t = x->right;
        t -= y->left;
        fenwick_increment(self->links, y->index, t);
        z = y;
        y = x;
        self->num_trapped_re_events++;
    }
    msp_insert_individual(self, z);
}

static void
msp_coancestry_event(msp_t *self)
{
    int eta;
    unsigned int j, n, l, r, r_max;
    avl_node_t *node;
    node_mapping_t *nm, search;
    segment_t *x, *y, *z, *alpha, *beta;

    self->num_ca_events++;
    /* Choose u and v */
    n = avl_count(self->population);
    j = gsl_rng_uniform_int(self->rng, n);
    node = avl_at(self->population, j);
    x = (segment_t *) node->item;
    avl_unlink_node(self->population, node);
    msp_free_avl_node(self, node);
    j = gsl_rng_uniform_int(self->rng, n - 1);
    node = avl_at(self->population, j);
    y = (segment_t *) node->item;
    avl_unlink_node(self->population, node);
    msp_free_avl_node(self, node);

    /* update num_links and get ready for loop */
    z = NULL;
    while (x != NULL || y != NULL) {
        alpha = NULL;
        if (x == NULL || y == NULL) {
            if (x != NULL) {
                alpha = x;
                x = NULL;
            }
            if (y != NULL) {
                alpha = y;
                y = NULL;
            }
        } else {
            if (y->left < x->left) {
                beta = x;
                x = y;
                y = beta;
            }
            if (x->right < y->left) {
                alpha = x;
                x = x->next;
                alpha->next = NULL;
            } else if (x->left != y->left) {
                alpha = msp_alloc_segment(self, x->left, y->left - 1, x->value,
                        NULL, NULL);
                x->left = y->left;
            } else {
                l = x->left;
                search.left = l;
                node = avl_search(self->breakpoints, &search);
                assert(node != NULL);
                nm = (node_mapping_t *) node->item;
                eta = nm->value;
                r_max = GSL_MIN(x->right, y->right);
                nm->value++;
                node = node->next;
                nm = (node_mapping_t *) node->item;
                r = nm->left;
                while (nm->value == eta && r < r_max) {
                    nm->value++;
                    node = node->next;
                    nm = (node_mapping_t *) node->item;
                    r = nm->left;
                }
                r--;
                msp_record_coalescence(self, l, r, x->value, y->value, eta);
                if (eta < 2 * self->sample_size - 1) {
                    alpha = msp_alloc_segment(self, l, r, eta, NULL, NULL);
                }
                if (x->right == r) {
                    beta = x;
                    x = x->next;
                    msp_free_segment(self, beta);
                } else {
                    x->left = r + 1;
                }
                if (y->right == r) {
                    beta = y;
                    y = y->next;
                    msp_free_segment(self, beta);
                } else {
                    y->left = r + 1;
                }
            }
        }
        if (alpha != NULL) {
            l = alpha->left;
            if (z == NULL) {
                msp_insert_individual(self, alpha);
            } else {
                z->next = alpha;
                if (self->approx == 0) {
                    l = z->right;
                }
            }
            alpha->prev = z;
            z = alpha;
            fenwick_set_value(self->links, alpha->index, alpha->right - l);
        }
    }
}


/*
 * Sets up the initial population and trees.
 */
static void
msp_initialise(msp_t *self)
{
    int j;
    segment_t *u;

    /* zero the counters */
    self->num_re_events = 0;
    self->num_ca_events = 0;
    self->num_coalescence_records = 0;
    self->num_trapped_re_events = 0;;
    self->max_used_segments = 0;
    self->max_population_size = 0;
    for (j = 1; j <= self->sample_size; j++) {
        u = msp_alloc_segment(self, 1, self->num_loci, j, NULL, NULL);
        msp_insert_individual(self, u);
        fenwick_increment(self->links, u->index, self->num_loci - 1);
    }
    msp_insert_breakpoint(self, 1, self->sample_size + 1);
    msp_insert_breakpoint(self, self->num_loci + 1, 0);
    self->time = 0.0;
    msp_open_coalescence_record_file(self);
}

static void
msp_finalise(msp_t *self)
{
    msp_flush_coalescence_records(self);
    if (self->coalescence_record_file != NULL) {
        msp_write_coalescence_record_file_footer(self);
        msp_close_coalescence_record_file(self);
    }
}

void
msp_print_detailed_stats(msp_t *self)
{
    unsigned int num_segments = 0;
    double mean_segment_length = 0.0;
    double mean_subtended_links = 0.0;
    avl_node_t *node;
    segment_t *u;

    for (node = self->population->head; node != NULL; node = node->next) {
        for (u = (segment_t *) node->item; u != NULL; u = u->next) {
            num_segments++;
            mean_segment_length += u->right - u->left;
            mean_subtended_links += fenwick_get_value(self->links, u->index);
        }
    }
    mean_segment_length /= num_segments;
    mean_subtended_links /= num_segments;

    printf("%d\t%llu\t%d\t%d\t%d\t%f\t%f\t%d\n", avl_count(self->population),
            fenwick_get_total(self->links), self->num_re_events, self->num_ca_events,
            num_segments, mean_segment_length, mean_subtended_links,
            self->num_trapped_re_events);
}

void
msp_simulate(msp_t *self)
{
    double lambda_c, lambda_r, t_c, t_r, t_wait, pop_size;
    long long num_links;
    unsigned int n = self->sample_size;
    void (*event_method)(msp_t *);
    population_model_t *pop_model = self->population_models;

    /* insert the sentinel population model */
    msp_add_constant_population_model(self, DBL_MAX, 0.0);
    assert(pop_model != NULL);
    msp_initialise(self);
    while (n > 1) {
        if (self->verbosity >= 2) {
            msp_print_state(self);
        }
        if (n > self->max_population_size) {
            self->max_population_size = n;
        }
        //msp_print_detailed_stats(self);
        num_links = fenwick_get_total(self->links);
        lambda_r = num_links * self->recombination_rate;
        t_r = DBL_MAX;
        if (lambda_r != 0.0) {
            t_r = gsl_ran_exponential(self->rng, 1.0 / lambda_r);
        }
        /* Need to perform n * (n - 1) as a double due to overflow */
        lambda_c = n * ((double) n - 1.0);
        t_c = pop_model->get_waiting_time(pop_model, lambda_c, self->time,
                self->rng);
        if (t_r < t_c) {
            t_wait = t_r;
            event_method = msp_recombination_event;
        } else {
            t_wait = t_c;
            event_method = msp_coancestry_event;
        }
        if (self->time + t_wait >= pop_model->next->start_time) {
            pop_size = pop_model->get_size(pop_model, self->time);
            pop_model = pop_model->next;
            pop_model->initial_size = pop_size;
            self->time = pop_model->start_time;
            assert(pop_model->next != NULL);
        } else {
            self->time += t_wait;
            event_method(self);
            n = avl_count(self->population);
        }
    }
    if (self->verbosity >= 1) {
        msp_print_state(self);
    }
    msp_finalise(self);
}


