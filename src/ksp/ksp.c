/*PGR-GNU*****************************************************************
File: ksp.c

Copyright (c) 2015 Celia Virginia Vergara Castillo
vicky_vergara@hotmail.com

------

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

 ********************************************************************PGR-GNU*/

#include <stdbool.h>
#include "c_common/postgres_connection.h"

#include "c_types/path_rt.h"
#include "c_common/debug_macro.h"
#include "c_common/e_report.h"
#include "c_common/time_msg.h"

#include "c_common/pgdata_getters.h"

#include "drivers/yen/ksp_driver.h"

PGDLLEXPORT Datum _pgr_ksp(PG_FUNCTION_ARGS);
PG_FUNCTION_INFO_V1(_pgr_ksp);

static
void compute(
        char* edges_sql,
        char* combinations_sql,
        ArrayType *starts,
        ArrayType *ends,
        int p_k,
        bool directed,
        bool heap_paths,
        Path_rt **result_tuples, size_t *result_count) {
    pgr_SPI_connect();
    char* log_msg = NULL;
    char* notice_msg = NULL;
    char* err_msg = NULL;
    if (p_k < 0) {
        return;
    }

    size_t k = (size_t)p_k;

    int64_t* start_vidsArr = NULL;
    size_t size_start_vidsArr = 0;

    int64_t* end_vidsArr = NULL;
    size_t size_end_vidsArr = 0;

    II_t_rt *combinations = NULL;
    size_t total_combinations = 0;

    if (starts && ends) {
        start_vidsArr = pgr_get_bigIntArray(&size_start_vidsArr, starts, false, &err_msg);
        throw_error(err_msg, "While getting start vids");
        end_vidsArr = pgr_get_bigIntArray(&size_end_vidsArr, ends, false, &err_msg);
        throw_error(err_msg, "While getting end vids");
    } else if (combinations_sql) {
        pgr_get_combinations(combinations_sql, &combinations, &total_combinations, &err_msg);
        throw_error(err_msg, combinations_sql);
    }

    PGR_DBG("Load data");
    Edge_t *edges = NULL;
    size_t total_edges = 0;


    // if (start_vertex == end_vertex) {
    //     pgr_SPI_finish();
    //     return;
    // }

    pgr_get_edges(edges_sql, &edges, &total_edges, true, false, &err_msg);
    throw_error(err_msg, edges_sql);
    PGR_DBG("Total %ld edges in query:", total_edges);

    if (total_edges == 0) {
        PGR_DBG("No edges found");
        pgr_SPI_finish();
        return;
    }


    PGR_DBG("Calling do_pgr_ksp\n");
    PGR_DBG("heap_paths = %i\n", heap_paths);

    clock_t start_t = clock();

    do_pgr_ksp(
            edges, total_edges,
            combinations, total_combinations,
            start_vidsArr, size_start_vidsArr,
            end_vidsArr, size_end_vidsArr,
            k,
            directed,
            heap_paths,
            result_tuples,
            result_count,
            &log_msg,
            &notice_msg,
            &err_msg);
    time_msg(" processing KSP", start_t, clock());

    if (err_msg && (*result_tuples)) {
        pfree(*result_tuples);
        (*result_tuples) = NULL;
        (*result_count) = 0;
    }

    pgr_global_report(log_msg, notice_msg, err_msg);

    if (log_msg) pfree(log_msg);
    if (notice_msg) pfree(notice_msg);
    if (err_msg) pfree(err_msg);

    if (start_vidsArr) pfree(start_vidsArr);
    if (end_vidsArr) pfree(end_vidsArr);


    pgr_global_report(log_msg, notice_msg, err_msg);

    pfree(edges);
    pgr_SPI_finish();
}


PGDLLEXPORT Datum
_pgr_ksp(PG_FUNCTION_ARGS) {
    FuncCallContext     *funcctx;
    TupleDesc            tuple_desc;
    Path_rt      *path = NULL;
    size_t result_count = 0;

    if (SRF_IS_FIRSTCALL()) {
        MemoryContext   oldcontext;
        funcctx = SRF_FIRSTCALL_INIT();
        oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);


        /*
           CREATE OR REPLACE FUNCTION _pgr_ksp(
           sql text,
           start_vid bigint,
           end_vid bigint,
           k integer,
           directed boolean,
           heap_paths boolean
           */
        PGR_DBG("Calling process");
        if (PG_NARGS() == 6){
            /*
             * many to many
             */
            compute(
                text_to_cstring(PG_GETARG_TEXT_P(0)),
                NULL,
                PG_GETARG_ARRAYTYPE_P(1),
                PG_GETARG_ARRAYTYPE_P(2),
                PG_GETARG_INT32(3),
                PG_GETARG_BOOL(4),
                PG_GETARG_BOOL(5),
                &path,
                &result_count);
        } else if (PG_NARGS() == 5){
            /*
             * combinations
             */
            compute(
                text_to_cstring(PG_GETARG_TEXT_P(0)),
                text_to_cstring(PG_GETARG_TEXT_P(1)),
                NULL,
                NULL,
                PG_GETARG_INT32(2),
                PG_GETARG_BOOL(3),
                PG_GETARG_BOOL(4),
                &path,
                &result_count);
        }

        PGR_DBG("Total number of tuples to be returned %ld \n", result_count);


        funcctx->max_calls = result_count;

        funcctx->user_fctx = path;
        if (get_call_result_type(fcinfo, NULL, &tuple_desc)
                != TYPEFUNC_COMPOSITE)
            ereport(ERROR,
                    (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                     errmsg("function returning record called in context "
                         "that cannot accept type record\n")));

        funcctx->tuple_desc = tuple_desc;
        MemoryContextSwitchTo(oldcontext);
    }


    funcctx = SRF_PERCALL_SETUP();


    tuple_desc = funcctx->tuple_desc;
    path = (Path_rt*) funcctx->user_fctx;

    if (funcctx->call_cntr < funcctx->max_calls) {
        HeapTuple    tuple;
        Datum        result;
        Datum *values;
        bool* nulls;

        values = palloc(9 * sizeof(Datum));
        nulls = palloc(9 * sizeof(bool));


        size_t i;
        for (i = 0; i < 9; ++i) {
            nulls[i] = false;
        }

        values[0] = Int32GetDatum(funcctx->call_cntr + 1);
        values[1] = Int32GetDatum(path[funcctx->call_cntr].start_id + 1);
        values[2] = Int32GetDatum(path[funcctx->call_cntr].seq);
        values[3] = Int64GetDatum(path[funcctx->call_cntr].start_id);
        values[4] = Int64GetDatum(path[funcctx->call_cntr].end_id);
        values[5] = Int64GetDatum(path[funcctx->call_cntr].node);
        values[6] = Int64GetDatum(path[funcctx->call_cntr].edge);
        values[7] = Float8GetDatum(path[funcctx->call_cntr].cost);
        values[8] = Float8GetDatum(path[funcctx->call_cntr].agg_cost);

        tuple = heap_form_tuple(tuple_desc, values, nulls);
        result = HeapTupleGetDatum(tuple);
        SRF_RETURN_NEXT(funcctx, result);
    } else {   /* do when there is no more left */
        SRF_RETURN_DONE(funcctx);
    }
}
