#include <Client/BuzzHouse/Generator/QueryOracle.h>

#include <cstdio>

namespace BuzzHouse
{

/*
Correctness query oracle
*/
/*
SELECT COUNT(*) FROM <FROM_CLAUSE> WHERE <PRED>;
or
SELECT COUNT(*) FROM <FROM_CLAUSE> WHERE <PRED1> GROUP BY <GROUP_BY CLAUSE> HAVING <PRED2>;
*/
int QueryOracle::generateCorrectnessTestFirstQuery(RandomGenerator & rg, StatementGenerator & gen, SQLQuery & sq1)
{
    TopSelect * ts = sq1.mutable_inner_query()->mutable_select();
    SelectIntoFile * sif = ts->mutable_intofile();
    SelectStatementCore * ssc = ts->mutable_sel()->mutable_select_core();
    const uint32_t combination = 0; //TODO fix this rg.nextLargeNumber() % 3; /* 0 WHERE, 1 HAVING, 2 WHERE + HAVING */

    can_test_query_success = rg.nextBool();
    gen.setAllowEngineUDF(!can_test_query_success);
    gen.setAllowNotDetermistic(false);
    gen.enforceFinal(true);
    gen.levels[gen.current_level] = QueryLevel(gen.current_level);
    gen.generateFromStatement(rg, std::numeric_limits<uint32_t>::max(), ssc->mutable_from());

    const bool prev_allow_aggregates = gen.levels[gen.current_level].allow_aggregates;
    const bool prev_allow_window_funcs = gen.levels[gen.current_level].allow_window_funcs;
    gen.levels[gen.current_level].allow_aggregates = gen.levels[gen.current_level].allow_window_funcs = false;
    if (combination != 1)
    {
        BinaryExpr * bexpr = ssc->mutable_where()->mutable_expr()->mutable_expr()->mutable_comp_expr()->mutable_binary_expr();

        bexpr->set_op(BinaryOperator::BINOP_EQ);
        bexpr->mutable_rhs()->mutable_lit_val()->set_special_val(SpecialVal::VAL_TRUE);
        gen.generateWherePredicate(rg, bexpr->mutable_lhs());
    }
    if (combination != 0)
    {
        gen.generateGroupBy(rg, 1, true, true, ssc->mutable_groupby());
    }
    gen.levels[gen.current_level].allow_aggregates = prev_allow_aggregates;
    gen.levels[gen.current_level].allow_window_funcs = prev_allow_window_funcs;

    ssc->add_result_columns()->mutable_eca()->mutable_expr()->mutable_comp_expr()->mutable_func_call()->mutable_func()->set_catalog_func(
        FUNCcount);
    gen.levels.erase(gen.current_level);
    gen.setAllowNotDetermistic(true);
    gen.enforceFinal(false);
    gen.setAllowEngineUDF(true);

    ts->set_format(OutFormat::OUT_CSV);
    sif->set_path(qfile.generic_string());
    sif->set_step(SelectIntoFile_SelectIntoFileStep::SelectIntoFile_SelectIntoFileStep_TRUNCATE);
    return 0;
}

/*
SELECT ifNull(SUM(PRED),0) FROM <FROM_CLAUSE>;
or
SELECT ifNull(SUM(PRED2),0) FROM <FROM_CLAUSE> WHERE <PRED1> GROUP BY <GROUP_BY CLAUSE>;
*/
int QueryOracle::generateCorrectnessTestSecondQuery(SQLQuery & sq1, SQLQuery & sq2)
{
    TopSelect * ts = sq2.mutable_inner_query()->mutable_select();
    SelectIntoFile * sif = ts->mutable_intofile();
    SelectStatementCore & ssc1 = const_cast<SelectStatementCore &>(sq1.inner_query().select().sel().select_core());
    SelectStatementCore * ssc2 = ts->mutable_sel()->mutable_select_core();
    SQLFuncCall * sfc1 = ssc2->add_result_columns()->mutable_eca()->mutable_expr()->mutable_comp_expr()->mutable_func_call();
    SQLFuncCall * sfc2 = sfc1->add_args()->mutable_expr()->mutable_comp_expr()->mutable_func_call();

    sfc1->mutable_func()->set_catalog_func(FUNCifNull);
    sfc1->add_args()->mutable_expr()->mutable_lit_val()->set_special_val(SpecialVal::VAL_ZERO);
    sfc2->mutable_func()->set_catalog_func(FUNCsum);

    ssc2->set_allocated_from(ssc1.release_from());
    if (ssc1.has_groupby())
    {
        GroupByStatement & gbs = const_cast<GroupByStatement &>(ssc1.groupby());

        sfc2->add_args()->set_allocated_expr(gbs.release_having_expr());
        ssc2->set_allocated_groupby(ssc1.release_groupby());
        ssc2->set_allocated_where(ssc1.release_where());
    }
    else
    {
        ExprComparisonHighProbability & expr = const_cast<ExprComparisonHighProbability &>(ssc1.where().expr());

        sfc2->add_args()->set_allocated_expr(expr.release_expr());
    }
    ts->set_format(OutFormat::OUT_CSV);
    sif->set_path(qfile.generic_string());
    sif->set_step(SelectIntoFile_SelectIntoFileStep::SelectIntoFile_SelectIntoFileStep_TRUNCATE);
    return 0;
}

/*
Dump and read table oracle
*/
int QueryOracle::dumpTableContent(RandomGenerator & rg, StatementGenerator & gen, const SQLTable & t, SQLQuery & sq1)
{
    bool first = true;
    TopSelect * ts = sq1.mutable_inner_query()->mutable_select();
    SelectIntoFile * sif = ts->mutable_intofile();
    SelectStatementCore * sel = ts->mutable_sel()->mutable_select_core();
    JoinedTable * jt = sel->mutable_from()->mutable_tos()->mutable_join_clause()->mutable_tos()->mutable_joined_table();
    OrderByList * obs = sel->mutable_orderby()->mutable_olist();
    ExprSchemaTable * est = jt->mutable_est();

    if (t.db)
    {
        est->mutable_database()->set_database("d" + std::to_string(t.db->dname));
    }
    est->mutable_table()->set_table("t" + std::to_string(t.tname));
    jt->set_final(t.supportsFinal());
    gen.flatTableColumnPath(0, t, [](const SQLColumn & c) { return c.canBeInserted(); });
    for (const auto & entry : gen.entries)
    {
        ExprOrderingTerm * eot = first ? obs->mutable_ord_term() : obs->add_extra_ord_terms();

        gen.columnPathRef(entry, sel->add_result_columns()->mutable_etc()->mutable_col()->mutable_path());
        gen.columnPathRef(entry, eot->mutable_expr()->mutable_comp_expr()->mutable_expr_stc()->mutable_col()->mutable_path());
        if (rg.nextBool())
        {
            eot->set_asc_desc(rg.nextBool() ? AscDesc::ASC : AscDesc::DESC);
        }
        if (rg.nextBool())
        {
            eot->set_nulls_order(
                rg.nextBool() ? ExprOrderingTerm_NullsOrder::ExprOrderingTerm_NullsOrder_FIRST
                              : ExprOrderingTerm_NullsOrder::ExprOrderingTerm_NullsOrder_LAST);
        }
        first = false;
    }
    gen.entries.clear();
    ts->set_format(OutFormat::OUT_CSV);
    sif->set_path(qfile.generic_string());
    sif->set_step(SelectIntoFile_SelectIntoFileStep::SelectIntoFile_SelectIntoFileStep_TRUNCATE);
    return 0;
}

static const std::map<OutFormat, InFormat> out_in{
    {OutFormat::OUT_CSV, InFormat::IN_CSV},
    {OutFormat::OUT_CSVWithNames, InFormat::IN_CSVWithNames},
    {OutFormat::OUT_CSVWithNamesAndTypes, InFormat::IN_CSVWithNamesAndTypes},
    {OutFormat::OUT_Values, InFormat::IN_Values},
    {OutFormat::OUT_JSON, InFormat::IN_JSON},
    {OutFormat::OUT_JSONColumns, InFormat::IN_JSONColumns},
    {OutFormat::OUT_JSONColumnsWithMetadata, InFormat::IN_JSONColumnsWithMetadata},
    {OutFormat::OUT_JSONCompact, InFormat::IN_JSONCompact},
    {OutFormat::OUT_JSONCompactColumns, InFormat::IN_JSONCompactColumns},
    {OutFormat::OUT_JSONEachRow, InFormat::IN_JSONEachRow},
    {OutFormat::OUT_JSONStringsEachRow, InFormat::IN_JSONStringsEachRow},
    {OutFormat::OUT_JSONCompactEachRow, InFormat::IN_JSONCompactEachRow},
    {OutFormat::OUT_JSONCompactEachRowWithNames, InFormat::IN_JSONCompactEachRowWithNames},
    {OutFormat::OUT_JSONCompactEachRowWithNamesAndTypes, InFormat::IN_JSONCompactEachRowWithNamesAndTypes},
    {OutFormat::OUT_JSONCompactStringsEachRow, InFormat::IN_JSONCompactStringsEachRow},
    {OutFormat::OUT_JSONCompactStringsEachRowWithNames, InFormat::IN_JSONCompactStringsEachRowWithNames},
    {OutFormat::OUT_JSONCompactStringsEachRowWithNamesAndTypes, InFormat::IN_JSONCompactStringsEachRowWithNamesAndTypes},
    {OutFormat::OUT_JSONObjectEachRow, InFormat::IN_JSONObjectEachRow},
    {OutFormat::OUT_BSONEachRow, InFormat::IN_BSONEachRow},
    {OutFormat::OUT_TSKV, InFormat::IN_TSKV},
    {OutFormat::OUT_Protobuf, InFormat::IN_Protobuf},
    {OutFormat::OUT_ProtobufSingle, InFormat::IN_ProtobufSingle},
    {OutFormat::OUT_Avro, InFormat::IN_Avro},
    {OutFormat::OUT_Parquet, InFormat::IN_Parquet},
    {OutFormat::OUT_Arrow, InFormat::IN_Arrow},
    {OutFormat::OUT_ArrowStream, InFormat::IN_ArrowStream},
    {OutFormat::OUT_ORC, InFormat::IN_ORC},
    {OutFormat::OUT_RowBinary, InFormat::IN_RowBinary},
    {OutFormat::OUT_RowBinaryWithNames, InFormat::IN_RowBinaryWithNames},
    {OutFormat::OUT_RowBinaryWithNamesAndTypes, InFormat::IN_RowBinaryWithNamesAndTypes},
    {OutFormat::OUT_Native, InFormat::IN_Native},
    {OutFormat::OUT_MsgPack, InFormat::IN_MsgPack}};

int QueryOracle::generateExportQuery(RandomGenerator & rg, StatementGenerator & gen, const SQLTable & t, SQLQuery & sq2)
{
    bool first = true;
    Insert * ins = sq2.mutable_inner_query()->mutable_insert();
    FileFunc * ff = ins->mutable_tfunction()->mutable_file();
    SelectStatementCore * sel = ins->mutable_insert_select()->mutable_select()->mutable_select_core();
    const std::filesystem::path & nfile = fc.db_file_path / "table.data";
    OutFormat outf = rg.pickKeyRandomlyFromMap(out_in);

    if (std::filesystem::exists(nfile))
    {
        (void)std::remove(nfile.generic_string().c_str()); //remove the file
    }
    ff->set_path(nfile.generic_string());

    buf.resize(0);
    gen.flatTableColumnPath(0, t, [](const SQLColumn & c) { return c.canBeInserted(); });
    for (const auto & entry : gen.entries)
    {
        SQLType * tp = entry.getBottomType();

        if (!first)
        {
            buf += ", ";
        }
        buf += entry.getBottomName();
        buf += " ";
        tp->typeName(buf, true);
        if (entry.nullable.has_value())
        {
            buf += entry.nullable.value() ? "" : " NOT";
            buf += " NULL";
        }
        gen.columnPathRef(entry, sel->add_result_columns()->mutable_etc()->mutable_col()->mutable_path());
        /* ArrowStream doesn't support UUID */
        if (outf == OutFormat::OUT_ArrowStream && dynamic_cast<UUIDType *>(tp))
        {
            outf = OutFormat::OUT_CSV;
        }
        first = false;
    }
    gen.entries.clear();
    ff->set_outformat(outf);
    ff->set_structure(buf);
    if (rg.nextSmallNumber() < 4)
    {
        ff->set_fcomp(static_cast<FileCompression>((rg.nextRandomUInt32() % static_cast<uint32_t>(FileCompression_MAX)) + 1));
    }

    //Set the table on select
    JoinedTable * jt = sel->mutable_from()->mutable_tos()->mutable_join_clause()->mutable_tos()->mutable_joined_table();
    ExprSchemaTable * est = jt->mutable_est();

    if (t.db)
    {
        est->mutable_database()->set_database("d" + std::to_string(t.db->dname));
    }
    est->mutable_table()->set_table("t" + std::to_string(t.tname));
    jt->set_final(t.supportsFinal());
    return 0;
}

int QueryOracle::generateClearQuery(const SQLTable & t, SQLQuery & sq3)
{
    Truncate * trunc = sq3.mutable_inner_query()->mutable_trunc();
    ExprSchemaTable * est = trunc->mutable_est();

    if (t.db)
    {
        est->mutable_database()->set_database("d" + std::to_string(t.db->dname));
    }
    est->mutable_table()->set_table("t" + std::to_string(t.tname));
    return 0;
}

int QueryOracle::generateImportQuery(StatementGenerator & gen, const SQLTable & t, const SQLQuery & sq2, SQLQuery & sq4)
{
    Insert * ins = sq4.mutable_inner_query()->mutable_insert();
    InsertFromFile * iff = ins->mutable_insert_file();
    const FileFunc & ff = sq2.inner_query().insert().tfunction().file();
    ExprSchemaTable * est = ins->mutable_est();

    if (t.db)
    {
        est->mutable_database()->set_database("d" + std::to_string(t.db->dname));
    }
    est->mutable_table()->set_table("t" + std::to_string(t.tname));
    gen.flatTableColumnPath(0, t, [](const SQLColumn & c) { return c.canBeInserted(); });
    for (const auto & entry : gen.entries)
    {
        gen.columnPathRef(entry, ins->add_cols());
    }
    gen.entries.clear();
    iff->set_path(ff.path());
    iff->set_format(out_in.at(ff.outformat()));
    if (ff.has_fcomp())
    {
        iff->set_fcomp(ff.fcomp());
    }
    if (iff->format() == IN_CSV)
    {
        SettingValues * vals = iff->mutable_settings();
        SetValue * sv = vals->mutable_set_value();

        sv->set_property("input_format_csv_detect_header");
        sv->set_value("0");
    }
    return 0;
}

static std::map<std::string, CHSetting> queryOracleSettings;

void loadFuzzerOracleSettings(const FuzzConfig &)
{
    for (auto & [key, value] : serverSettings)
    {
        if (!value.oracle_values.empty())
        {
            queryOracleSettings.insert({{key, value}});
        }
    }
}

/*
Run query with different settings oracle
*/
int QueryOracle::generateFirstSetting(RandomGenerator & rg, SQLQuery & sq1)
{
    const uint32_t nsets = rg.nextBool() ? 1 : ((rg.nextSmallNumber() % 3) + 1);
    SettingValues * sv = sq1.mutable_inner_query()->mutable_setting_values();

    nsettings.clear();
    for (uint32_t i = 0; i < nsets; i++)
    {
        const std::string & setting = rg.pickKeyRandomlyFromMap(queryOracleSettings);
        const CHSetting & chs = queryOracleSettings.at(setting);
        SetValue * setv = i == 0 ? sv->mutable_set_value() : sv->add_other_values();

        setv->set_property(setting);
        if (chs.oracle_values.size() == 2)
        {
            if (rg.nextBool())
            {
                setv->set_value(*chs.oracle_values.begin());
                nsettings.push_back(*std::next(chs.oracle_values.begin(), 1));
            }
            else
            {
                setv->set_value(*std::next(chs.oracle_values.begin(), 1));
                nsettings.push_back(*(chs.oracle_values.begin()));
            }
        }
        else
        {
            setv->set_value(rg.pickRandomlyFromSet(chs.oracle_values));
            nsettings.push_back(rg.pickRandomlyFromSet(chs.oracle_values));
        }
    }
    return 0;
}

int QueryOracle::generateSecondSetting(const SQLQuery & sq1, SQLQuery & sq3)
{
    const SettingValues & osv = sq1.inner_query().setting_values();
    SettingValues * sv = sq3.mutable_inner_query()->mutable_setting_values();

    for (size_t i = 0; i < nsettings.size(); i++)
    {
        const SetValue & osetv = i == 0 ? osv.set_value() : osv.other_values(static_cast<int>(i - 1));
        SetValue * setv = i == 0 ? sv->mutable_set_value() : sv->add_other_values();

        setv->set_property(osetv.property());
        setv->set_value(nsettings[i]);
    }
    return 0;
}

int QueryOracle::generateOracleSelectQuery(RandomGenerator & rg, const bool peer_query, StatementGenerator & gen, SQLQuery & sq2)
{
    TopSelect * ts = sq2.mutable_inner_query()->mutable_select();
    SelectIntoFile * sif = ts->mutable_intofile();
    const bool global_aggregate = rg.nextSmallNumber() < 4;

    gen.setAllowNotDetermistic(false);
    gen.enforceFinal(true);
    gen.generatingPeerQuery(peer_query);
    gen.generateTopSelect(rg, global_aggregate, std::numeric_limits<uint32_t>::max(), ts);
    gen.setAllowNotDetermistic(true);
    gen.enforceFinal(false);
    gen.generatingPeerQuery(false);

    if (!global_aggregate)
    {
        //if not global aggregate, use ORDER BY clause
        Select * osel = ts->release_sel();
        SelectStatementCore * nsel = ts->mutable_sel()->mutable_select_core();
        nsel->mutable_from()->mutable_tos()->mutable_join_clause()->mutable_tos()->mutable_joined_derived_query()->set_allocated_select(
            osel);
        nsel->mutable_orderby()->set_oall(true);
    }
    ts->set_format(OutFormat::OUT_CSV);
    sif->set_path(qfile.generic_string());
    sif->set_step(SelectIntoFile_SelectIntoFileStep::SelectIntoFile_SelectIntoFileStep_TRUNCATE);
    return 0;
}

void QueryOracle::findTablesWithPeersAndReplace(RandomGenerator & rg, google::protobuf::Message & mes, StatementGenerator & gen)
{
    checkStackSize();

    if (mes.GetTypeName() == "BuzzHouse.Select")
    {
        auto & sel = static_cast<Select &>(mes);

        if (sel.has_select_core())
        {
            findTablesWithPeersAndReplace(rg, const_cast<SelectStatementCore &>(sel.select_core()), gen);
        }
        else if (sel.has_set_query())
        {
            findTablesWithPeersAndReplace(rg, const_cast<SetQuery &>(sel.set_query()), gen);
        }
        if (sel.has_ctes())
        {
            findTablesWithPeersAndReplace(rg, const_cast<Select &>(sel.ctes().cte().query()), gen);
            for (int i = 0; i < sel.ctes().other_ctes_size(); i++)
            {
                findTablesWithPeersAndReplace(rg, const_cast<Select &>(sel.ctes().other_ctes(i).query()), gen);
            }
        }
    }
    else if (mes.GetTypeName() == "BuzzHouse.SetQuery")
    {
        auto & setq = static_cast<SetQuery &>(mes);

        findTablesWithPeersAndReplace(rg, const_cast<Select &>(setq.sel1()), gen);
        findTablesWithPeersAndReplace(rg, const_cast<Select &>(setq.sel2()), gen);
    }
    else if (mes.GetTypeName() == "BuzzHouse.SelectStatementCore")
    {
        auto & ssc = static_cast<SelectStatementCore &>(mes);

        if (ssc.has_from())
        {
            findTablesWithPeersAndReplace(rg, const_cast<JoinedQuery &>(ssc.from().tos()), gen);
        }
    }
    else if (mes.GetTypeName() == "BuzzHouse.JoinedQuery")
    {
        auto & jquery = static_cast<JoinedQuery &>(mes);

        for (int i = 0; i < jquery.tos_list_size(); i++)
        {
            findTablesWithPeersAndReplace(rg, const_cast<TableOrSubquery &>(jquery.tos_list(i)), gen);
        }
        findTablesWithPeersAndReplace(rg, const_cast<JoinClause &>(jquery.join_clause()), gen);
    }
    else if (mes.GetTypeName() == "BuzzHouse.JoinClause")
    {
        auto & jclause = static_cast<JoinClause &>(mes);

        for (int i = 0; i < jclause.clauses_size(); i++)
        {
            if (jclause.clauses(i).has_core())
            {
                findTablesWithPeersAndReplace(rg, const_cast<TableOrSubquery &>(jclause.clauses(i).core().tos()), gen);
            }
        }
        findTablesWithPeersAndReplace(rg, const_cast<TableOrSubquery &>(jclause.tos()), gen);
    }
    else if (mes.GetTypeName() == "BuzzHouse.TableOrSubquery")
    {
        auto & tos = static_cast<TableOrSubquery &>(mes);

        if (tos.has_joined_table())
        {
            const ExprSchemaTable & est = tos.joined_table().est();

            if ((!est.has_database() || est.database().database() != "system") && est.table().table().at(0) == 't')
            {
                const uint32_t tname = static_cast<uint32_t>(std::stoul(tos.joined_table().est().table().table().substr(1)));

                if (gen.tables.find(tname) != gen.tables.end())
                {
                    const SQLTable & t = gen.tables.at(tname);

                    if (t.hasDatabasePeer())
                    {
                        buf.resize(0);
                        buf += tos.joined_table().table_alias().table();
                        tos.clear_joined_table();
                        JoinedTableFunction * jtf = tos.mutable_joined_table_function();
                        gen.setTableRemote<false>(rg, t, jtf->mutable_tfunc());
                        jtf->mutable_table_alias()->set_table(buf);
                        found_tables.insert(tname);
                        can_test_query_success &= t.hasClickHousePeer();
                    }
                }
            }
        }
        else if (tos.has_joined_derived_query())
        {
            findTablesWithPeersAndReplace(rg, const_cast<Select &>(tos.joined_derived_query().select()), gen);
        }
        else if (tos.has_joined_query())
        {
            findTablesWithPeersAndReplace(rg, const_cast<JoinedQuery &>(tos.joined_query()), gen);
        }
    }
}

int QueryOracle::truncatePeerTables(const StatementGenerator & gen) const
{
    for (const auto & entry : found_tables)
    {
        //first truncate tables
        gen.connections.truncatePeerTableOnRemote(gen.tables.at(entry));
    }
    return 0;
}

int QueryOracle::optimizePeerTables(const StatementGenerator & gen) const
{
    for (const auto & entry : found_tables)
    {
        //lastly optimize tables
        gen.connections.optimizePeerTableOnRemote(gen.tables.at(entry));
    }
    return 0;
}

int QueryOracle::replaceQueryWithTablePeers(
    RandomGenerator & rg, const SQLQuery & sq1, StatementGenerator & gen, std::vector<SQLQuery> & peer_queries, SQLQuery & sq2)
{
    found_tables.clear();
    peer_queries.clear();

    sq2.CopyFrom(sq1);
    findTablesWithPeersAndReplace(rg, const_cast<Select &>(sq2.inner_query().select().sel()), gen);
    for (const auto & entry : found_tables)
    {
        SQLQuery next;
        const SQLTable & t = gen.tables.at(entry);
        Insert * ins = next.mutable_inner_query()->mutable_insert();
        SelectStatementCore * sel = ins->mutable_insert_select()->mutable_select()->mutable_select_core();

        //then insert
        gen.setTableRemote<false>(rg, t, ins->mutable_tfunction());
        JoinedTable * jt = sel->mutable_from()->mutable_tos()->mutable_join_clause()->mutable_tos()->mutable_joined_table();
        ExprSchemaTable * est = jt->mutable_est();
        if (t.db)
        {
            est->mutable_database()->set_database("d" + std::to_string(t.db->dname));
        }
        est->mutable_table()->set_table("t" + std::to_string(t.tname));
        jt->set_final(t.supportsFinal());
        gen.flatTableColumnPath(0, t, [](const SQLColumn & c) { return c.canBeInserted(); });
        for (const auto & colRef : gen.entries)
        {
            gen.columnPathRef(colRef, ins->add_cols());
            gen.columnPathRef(colRef, sel->add_result_columns()->mutable_etc()->mutable_col()->mutable_path());
        }
        gen.entries.clear();
        peer_queries.push_back(std::move(next));
    }
    return 0;
}

int QueryOracle::ResetOracleValues()
{
    first_success = second_sucess = other_steps_sucess = can_test_query_success = true;
    return 0;
}

int QueryOracle::SetIntermediateStepSuccess(const bool success)
{
    other_steps_sucess &= success;
    return 0;
}

int QueryOracle::processFirstOracleQueryResult(const bool success)
{
    if (success)
    {
        md5_hash.hashFile(qfile.generic_string(), first_digest);
    }
    first_success = success;
    return 0;
}

int QueryOracle::processSecondOracleQueryResult(const bool success, const std::string & oracle_name)
{
    if (success)
    {
        md5_hash.hashFile(qfile.generic_string(), second_digest);
    }
    second_sucess = success;
    if (other_steps_sucess)
    {
        if (can_test_query_success && first_success != second_sucess)
        {
            throw std::runtime_error(oracle_name + " oracle failed with different success results");
        }
        if (first_success && second_sucess && !std::equal(std::begin(first_digest), std::end(first_digest), std::begin(second_digest)))
        {
            throw std::runtime_error(oracle_name + " oracle failed with different result sets");
        }
    }
    return 0;
}

}
