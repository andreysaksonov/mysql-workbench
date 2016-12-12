/*
 * Copyright (c) 2015, 2016, Oracle and/or its affiliates. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; version 2 of the
 * License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301  USA
 */ 

#ifndef HAVE_PRECOMPILED_HEADERS
  #include <fstream>
  #include <string>
  #include <vector>
  #include <map>
  #include <set>
  #include <deque>

  #include "antlr4-runtime.h"
  #include <glib.h>
#endif

#include "base/common.h"
#include "base/log.h"
#include "base/file_utilities.h"
#include "base/string_utilities.h"
#include "base/threading.h"

#include "parsers-common.h"
#include "MySQLLexer.h"
#include "MySQLParser.h"
#include "MySQLParserBaseListener.h"
#include "CodeCompletionCore.h"

#include "mysql_object_names_cache.h"

#include "mysql-code-completion.h"

using namespace parsers;
using namespace antlr4;

DEFAULT_LOG_DOMAIN("MySQL code completion");

//--------------------------------------------------------------------------------------------------

struct TableReference
{
  std::string schema;
  std::string table;
  std::string alias;
};

// Context structure for code completion results and token info.
struct AutoCompletionContext
{
  enum RunState { RunStateMatching, RunStateCollectionPending } runState;

  CandidatesCollection completionCandidates;

  // A hierarchical view of all table references in the code, updated constantly during the match process.
  // Organized as stack to be able to easily remove sets of references when changing nesting level.
  std::deque<std::vector<TableReference>> referencesStack;

  // A flat list of possible references. Kinda snapshot of the references stack at the point when collection
  // begins (the stack is cleaned up while bubbling up, after the collection process).
  // Additionally, it gets also all references after the caret.
  std::vector<TableReference> references;

  //------------------------------------------------------------------------------------------------

  /**
  * Uses the given scanner (with set input) to collect a set of possible completion candidates
  * at the given line + offset.
  *
  * @returns true if the input could fully be matched (happens usually only if the given caret
  *          is after the text and can be used to test if the algorithm parses queries fully).
  *
  * Actual candidates are stored in the completionCandidates member set.
  *
  */
  void collectCandidates(Parser *parser, std::pair<size_t, size_t> caret)
  {
    CodeCompletionCore c3(parser);

    c3.ignoredTokens = {
      MySQLLexer::EOF,
      MySQLLexer::EQUAL_OPERATOR,
      MySQLLexer::ASSIGN_OPERATOR,
      MySQLLexer::NULL_SAFE_EQUAL_OPERATOR,
      MySQLLexer::GREATER_OR_EQUAL_OPERATOR,
      MySQLLexer::GREATER_THAN_OPERATOR,
      MySQLLexer::LESS_OR_EQUAL_OPERATOR,
      MySQLLexer::LESS_THAN_OPERATOR,
      MySQLLexer::NOT_EQUAL_OPERATOR,
      MySQLLexer::NOT_EQUAL2_OPERATOR,
      MySQLLexer::PLUS_OPERATOR,
      MySQLLexer::MINUS_OPERATOR,
      MySQLLexer::MULT_OPERATOR,
      MySQLLexer::DIV_OPERATOR,
      MySQLLexer::MOD_OPERATOR,
      MySQLLexer::LOGICAL_NOT_OPERATOR,
      MySQLLexer::BITWISE_NOT_OPERATOR,
      MySQLLexer::SHIFT_LEFT_OPERATOR,
      MySQLLexer::SHIFT_RIGHT_OPERATOR,
      MySQLLexer::LOGICAL_AND_OPERATOR,
      MySQLLexer::BITWISE_AND_OPERATOR,
      MySQLLexer::BITWISE_XOR_OPERATOR,
      MySQLLexer::LOGICAL_OR_OPERATOR,
      MySQLLexer::BITWISE_OR_OPERATOR,
      MySQLLexer::DOT_SYMBOL,
      MySQLLexer::COMMA_SYMBOL,
      MySQLLexer::SEMICOLON_SYMBOL,
      MySQLLexer::COLON_SYMBOL,
      MySQLLexer::OPEN_PAR_SYMBOL,
      MySQLLexer::CLOSE_PAR_SYMBOL,
      MySQLLexer::OPEN_CURLY_SYMBOL,
      MySQLLexer::CLOSE_CURLY_SYMBOL,
      MySQLLexer::UNDERLINE_SYMBOL,
      MySQLLexer::AT_SIGN_SYMBOL,
      MySQLLexer::AT_AT_SIGN_SYMBOL,
      MySQLLexer::NULL2_SYMBOL,
      MySQLLexer::PARAM_MARKER,
      MySQLLexer::CONCAT_PIPES_SYMBOL,
      MySQLLexer::AT_TEXT_SUFFIX,
      MySQLLexer::BACK_TICK_QUOTED_ID,
      MySQLLexer::SINGLE_QUOTED_TEXT,
      MySQLLexer::DOUBLE_QUOTED_TEXT,
      MySQLLexer::NCHAR_TEXT,
      MySQLLexer::UNDERSCORE_CHARSET,
      MySQLLexer::IDENTIFIER,
      MySQLLexer::INT_NUMBER,
      MySQLLexer::LONG_NUMBER,
      MySQLLexer::ULONGLONG_NUMBER,
      MySQLLexer::DECIMAL_NUMBER,
      MySQLLexer::BIN_NUMBER,
      MySQLLexer::HEX_NUMBER,
    };

    c3.preferredRules = {
      MySQLParser::RuleSchemaRef,

      MySQLParser::RuleTableRef,
      MySQLParser::RuleTableRefWithWildcard,
      MySQLParser::RuleFilterTableRef,
      MySQLParser::RuleTableRefNoDb,

      MySQLParser::RuleColumnRef,
      MySQLParser::RuleColumnInternalRef,
      MySQLParser::RuleTableWild,

      MySQLParser::RuleFunctionRef,
      MySQLParser::RuleFunctionCall,
      MySQLParser::RuleRuntimeFunctionCall,
      MySQLParser::RuleTriggerRef,
      MySQLParser::RuleViewRef,
      MySQLParser::RuleProcedureRef,
      MySQLParser::RuleLogfileGroupRef,
      MySQLParser::RuleTablespaceRef,
      MySQLParser::RuleEngineRef,
      MySQLParser::RuleCollationName,
      MySQLParser::RuleCharsetName,
      MySQLParser::RuleEventRef,
      MySQLParser::RuleServerRef,

      MySQLParser::RuleUserVariable,
      MySQLParser::RuleSystemVariable,
      MySQLParser::RuleLabelRef,

      // For better handling, but will be ignored.
      MySQLParser::RuleParameterName,
      MySQLParser::RuleProcedureName,
      MySQLParser::RuleIdentifier,
      MySQLParser::RuleLabelIdentifier,
    };

    c3.noSeparatorRequiredFor = {
      MySQLLexer::EQUAL_OPERATOR,
      MySQLLexer::ASSIGN_OPERATOR,
      MySQLLexer::NULL_SAFE_EQUAL_OPERATOR,
      MySQLLexer::GREATER_OR_EQUAL_OPERATOR,
      MySQLLexer::GREATER_THAN_OPERATOR,
      MySQLLexer::LESS_OR_EQUAL_OPERATOR,
      MySQLLexer::LESS_THAN_OPERATOR,
      MySQLLexer::NOT_EQUAL_OPERATOR,
      MySQLLexer::NOT_EQUAL2_OPERATOR,
      MySQLLexer::PLUS_OPERATOR,
      MySQLLexer::MINUS_OPERATOR,
      MySQLLexer::MULT_OPERATOR,
      MySQLLexer::DIV_OPERATOR,
      MySQLLexer::MOD_OPERATOR,
      MySQLLexer::LOGICAL_NOT_OPERATOR,
      MySQLLexer::BITWISE_NOT_OPERATOR,
      MySQLLexer::SHIFT_LEFT_OPERATOR,
      MySQLLexer::SHIFT_RIGHT_OPERATOR,
      MySQLLexer::LOGICAL_AND_OPERATOR,
      MySQLLexer::BITWISE_AND_OPERATOR,
      MySQLLexer::BITWISE_XOR_OPERATOR,
      MySQLLexer::LOGICAL_OR_OPERATOR,
      MySQLLexer::BITWISE_OR_OPERATOR,
      MySQLLexer::DOT_SYMBOL,
      MySQLLexer::COMMA_SYMBOL,
      MySQLLexer::SEMICOLON_SYMBOL,
      MySQLLexer::COLON_SYMBOL,
      MySQLLexer::OPEN_PAR_SYMBOL,
      MySQLLexer::CLOSE_PAR_SYMBOL,
      MySQLLexer::OPEN_CURLY_SYMBOL,
      MySQLLexer::CLOSE_CURLY_SYMBOL,
      MySQLLexer::PARAM_MARKER,
    };
    
    c3.showResult = true;
    referencesStack.push_back(std::vector<TableReference>()); // For the root level of table references.
    completionCandidates = c3.collectCandidates(caret);

    // Post processing some entries.
    if (completionCandidates.tokens.count(MySQLLexer::NOT2_SYMBOL) > 0)
    {
      // NOT2 is a NOT with special meaning in the operator precedence chain.
      // For code completion it's the same as NOT.
      completionCandidates.tokens[MySQLLexer::NOT_SYMBOL] = completionCandidates.tokens[MySQLLexer::NOT2_SYMBOL];
      completionCandidates.tokens.erase(MySQLLexer::NOT2_SYMBOL);
    }

    // If a column reference is required then we have to continue scanning the query for table references.
    for (auto ruleEntry : completionCandidates.rules) {
      if (ruleEntry.first == MySQLParser::RuleColumnRef)
      {
        collectRemainingTableReferences();
        takeReferencesSnapshot(); // Move references from stack to the ref map.
      }
    }

    return;
  }

  //------------------------------------------------------------------------------------------------

private:
  /**
   * Called if one of the candidates is a column reference.
   * The function attempts to get table references together with aliases where possible.
   * Because inner queries can use table references from outer queries we can simply scan for any FROM clause
   * provided we don't go deeper. This way the query doesn't need to be error free, just the FROM clauses must.
  */
  void collectRemainingTableReferences()
  {
    // A listener to handle references as we traverse the subtree produced by the tablekeyList rule.
    class TableRefListener : public parsers::MySQLParserBaseListener {
    public:

      virtual void exitDataType(MySQLParser::DataTypeContext *ctx) override {

      }

    private:
     std::size_t _level = 0;

    };

   
    // First advance to the FROM keyword on the same level as the caret is (no subselects etc.).
    // With certain syntax errors this can lead to a wrong FROM clause (e.g. if parentheses don't match).
    // But that is acceptable.

    // Reset the scanner to the caret position and continue from there. We have already collected all
    // table references before that position during normal matching.

  }

  //------------------------------------------------------------------------------------------------

  /**
   * Copies the current references stack into the references map.
   */
  void takeReferencesSnapshot()
  {
    // Don't clear the references map here. Can happen we have to take multiple snapshots.
    // We automatically remove duplicates by using a map.
    for (size_t level = 0; level < referencesStack.size(); ++level)
    {
      for (size_t entry = 0; entry < referencesStack[level].size(); ++entry)
        references.push_back(referencesStack[level][entry]);
    }

  }

  //------------------------------------------------------------------------------------------------

};

//--------------------------------------------------------------------------------------------------

enum ObjectFlags {
  // For 3 part identifiers.
  ShowSchemas = 1 << 0,
  ShowTables = 1 << 1,
  ShowColumns = 1 << 2,

  // For 2 part identifiers.
  ShowFirst = 1 << 3,
  ShowSecond = 1 << 4,
};

/**
 * Determines the qualifier used for a qualified identifier with up to 2 parts (id or id.id).
 * Returns the found qualifier (if any) and a flag indicating what should be shown.
 *
 * Note: it is essential to understand that we do the determination only up to the caret
 *       (or the token following it, solely for getting a terminator). Since we cannot know the user's
 *       intention, we never look forward.
 */
static ObjectFlags determineQualifier(Scanner &scanner, MySQLLexer *lexer, size_t offsetInLine, std::string &qualifier) {
  // Five possible positions here:
  //   - In the first id (including the position directly after the last char).
  //   - In the space between first id and a dot.
  //   - On a dot (visually directly before the dot).
  //   - In space after the dot, that includes the position directly after the dot.
  //   - In the second id.
  // All parts are optional (though not at the same time). The on-dot position is considered the same
  // as in first id as it visually belongs to the first id.

  size_t position = scanner.tokenIndex();

  if (scanner.tokenChannel() != 0)
    scanner.next(true); // First skip to the next non-hidden token.

  if (!scanner.is(MySQLLexer::DOT_SYMBOL) && !lexer->isIdentifier(scanner.tokenType())) {
    // We are at the end of an incomplete identifier spec. Jump back, so that the other tests succeed.
    scanner.previous(true);
  }

  // Go left until we find something not related to an id or find at most 1 dot.
  if (position > 0) {
    if (lexer->isIdentifier(scanner.tokenType()) && scanner.lookBack() == MySQLLexer::DOT_SYMBOL)
      scanner.previous(true);
    if (scanner.is(MySQLLexer::DOT_SYMBOL) && lexer->isIdentifier(scanner.lookBack()))
      scanner.previous(true);
  }

  // The scanner is now on the leading identifier or dot (if there's no leading id).
  qualifier = "";
  std::string temp;
  if (lexer->isIdentifier(scanner.tokenType())) {
    temp = base::unquote(scanner.tokenText());
    scanner.next(true);
  }

  // Bail out if there is no more id parts or we are already behind the caret position.
  if (!scanner.is(MySQLLexer::DOT_SYMBOL) || position <= scanner.tokenIndex())
    return ObjectFlags(ShowFirst | ShowSecond);
  qualifier = temp;

  return ShowSecond;
}

//--------------------------------------------------------------------------------------------------

/**
 * Enhanced variant of the previous function that determines schema and table qualifiers for
 * column references (and table_wild in multi table delete, for that matter).
 * Returns a set of flags that indicate what to show for that identifier, as well as schema and table
 * if given.
 * The returned schema can be either for a schema.table situation (which requires to show tables)
 * or a schema.table.column situation. Which one is determined by whether showing columns alone or not.
 */
static ObjectFlags determineSchemaTableQualifier(Scanner &scanner, MySQLLexer *lexer, std::string &schema,
  std::string &table) {

  size_t position = scanner.tokenIndex();
  if (scanner.tokenChannel() != 0)
    scanner.next(true);

  size_t tokenType = scanner.tokenType();
  if (tokenType != MySQLLexer::DOT_SYMBOL && !lexer->isIdentifier(scanner.tokenType()))
  {
    // Just like in the simpler function. If we have found no identifier or dot then we are at the
    // end of an incomplete definition. Simply seek back to the previous non-hidden token.
    scanner.previous(true);
  }

  // Go left until we find something not related to an id or at most 2 dots.
  if (position > 0) {
    if (lexer->isIdentifier(scanner.tokenType()) && (scanner.lookBack() == MySQLLexer::DOT_SYMBOL))
      scanner.previous(true);
    if (scanner.is(MySQLLexer::DOT_SYMBOL) && lexer->isIdentifier(scanner.lookBack())) {
      scanner.previous(true);

      // And once more.
      if (scanner.lookBack() == MySQLLexer::DOT_SYMBOL) {
        scanner.previous(true);
        if (lexer->isIdentifier(scanner.lookBack()))
          scanner.previous(true);
      }
    }
  }

  // The scanner is now on the leading identifier or dot (if there's no leading id).
  schema = "";
  table = "";

  std::string temp;
  if (lexer->isIdentifier(scanner.tokenType())) {
    temp = base::unquote(scanner.tokenText());
    scanner.next(true);
  }

  // Bail out if there is no more id parts or we are already behind the caret position.
  if (!scanner.is(MySQLLexer::DOT_SYMBOL) || position <= scanner.tokenIndex())
    return ObjectFlags(ShowSchemas | ShowTables | ShowColumns);

  scanner.next(true); // Skip dot.
  table = temp;
  schema = temp;
  if (lexer->isIdentifier(scanner.tokenType())) {
    temp = base::unquote(scanner.tokenText());
    scanner.next(true);

    if (!scanner.is(MySQLLexer::DOT_SYMBOL) || position <= scanner.tokenIndex())
      return ObjectFlags(ShowTables | ShowColumns); // Schema only valid for tables. Columns must use default schema.

    table = temp;
    return ShowColumns;
  }

  return ObjectFlags(ShowTables | ShowColumns); // Schema only valid for tables. Columns must use default schema.
}

//--------------------------------------------------------------------------------------------------

struct CompareAcEntries
{
  bool operator() (const std::pair<int, std::string> &lhs, const std::pair<int, std::string> &rhs) const
  {
    return base::string_compare(lhs.second, rhs.second, false) < 0;
  }
};

typedef std::set<std::pair<int, std::string>, CompareAcEntries> CompletionSet;

//--------------------------------------------------------------------------------------------------

static void insertSchemas(MySQLObjectNamesCache *cache, CompletionSet &set, const std::string &typedPart)
{
  std::vector<std::string> schemas = cache->getMatchingSchemaNames(typedPart);
  for (auto &schema : schemas)
    set.insert({AC_SCHEMA_IMAGE, schema});
}

//--------------------------------------------------------------------------------------------------

static void insertTables(MySQLObjectNamesCache *cache, CompletionSet &set, std::set<std::string> &schemas,
  const std::string &typedPart)
{
  for (auto &schema : schemas)
  {
    std::vector<std::string> tables = cache->getMatchingTableNames(schema, typedPart);
    for (auto &table : tables)
      set.insert({AC_TABLE_IMAGE, table});
    }
}

//--------------------------------------------------------------------------------------------------

static void insertViews(MySQLObjectNamesCache *cache, CompletionSet &set, const std::set<std::string> &schemas,
  const std::string &typedPart)
{
  for (auto &schema : schemas)
  {
    std::vector<std::string> views = cache->getMatchingViewNames(schema, typedPart);
    for (auto &view : views)
      set.insert({AC_VIEW_IMAGE, view});
  }
}

//--------------------------------------------------------------------------------------------------

static void insertColumns(MySQLObjectNamesCache *cache, CompletionSet &set, const std::set<std::string> &schemas,
  const std::set<std::string> &tables, const std::string &typedPart)
{
  for (auto &schema : schemas)
  {
    for (auto &table : tables)
    {
      std::vector<std::string> columns = cache->getMatchingColumnNames(schema, table, typedPart);
      for (auto &column : columns)
        set.insert({AC_COLUMN_IMAGE, column});
    }
  }
}

//--------------------------------------------------------------------------------------------------

std::vector<std::pair<int, std::string>> getCodeCompletionList(size_t caretLine, size_t caretOffset,
  const std::string &defaultSchema, bool uppercaseKeywords, MySQLParser *parser, const std::string &functionNames,
  MySQLObjectNamesCache *cache)
{
  logDebug("Invoking code completion\n");

  AutoCompletionContext context;

  // A set for each object type. This will sort the groups alphabetically and avoids duplicates,
  // but allows to add them as groups to the final list.
  CompletionSet schemaEntries;
  CompletionSet tableEntries;
  CompletionSet columnEntries;
  CompletionSet viewEntries;
  CompletionSet functionEntries;
  CompletionSet udfEntries;
  CompletionSet runtimeFunctionEntries;
  CompletionSet procedureEntries;
  CompletionSet triggerEntries;
  CompletionSet engineEntries;
  CompletionSet logfileGroupEntries;
  CompletionSet tablespaceEntries;
  CompletionSet systemVarEntries;
  CompletionSet keywordEntries;
  CompletionSet collationEntries;
  CompletionSet charsetEntries;
  CompletionSet eventEntries;

  // Handled but needs meat yet.
  CompletionSet userVarEntries;
  
  // To be done yet.
  CompletionSet userEntries;
  CompletionSet indexEntries;
  CompletionSet pluginEntries;
  CompletionSet fkEntries;

  static std::map<size_t, std::vector<std::string>> synonyms = {
    { MySQLLexer::CHAR_SYMBOL, { "CHARACTER" }},
    { MySQLLexer::NOW_SYMBOL, { "CURRENT_TIMESTAMP", "LOCALTIME", "LOCALTIMESTAMP" }},
    { MySQLLexer::DAY_SYMBOL, { "DAYOFMONTH" }},
    { MySQLLexer::DECIMAL_SYMBOL, { "DEC" }},
    { MySQLLexer::DISTINCT_SYMBOL, { "DISTINCTROW" }},
    { MySQLLexer::CHAR_SYMBOL, { "CHARACTER" }},
    { MySQLLexer::COLUMNS_SYMBOL, { "FIELDS" }},
    { MySQLLexer::FLOAT_SYMBOL, { "FLOAT4" }},
    { MySQLLexer::DOUBLE_SYMBOL, { "FLOAT8" }},
    { MySQLLexer::INT_SYMBOL, { "INTEGER", "INT4" }},
    { MySQLLexer::RELAY_THREAD_SYMBOL, { "IO_THREAD" }},
    { MySQLLexer::SUBSTRING_SYMBOL, { "MID" }},
    { MySQLLexer::MID_SYMBOL, { "MEDIUMINT" }},
    { MySQLLexer::MEDIUMINT_SYMBOL, { "MIDDLEINT" }},
    { MySQLLexer::NDBCLUSTER_SYMBOL, { "NDB" }},
    { MySQLLexer::REGEXP_SYMBOL, { "RLIKE" }},
    { MySQLLexer::DATABASE_SYMBOL, { "SCHEMA" }},
    { MySQLLexer::DATABASES_SYMBOL, { "SCHEMAS" }},
    { MySQLLexer::USER_SYMBOL, { "SESSION_USER" }},
    { MySQLLexer::STD_SYMBOL, { "STDDEV", "STDDEV" }},
    { MySQLLexer::SUBSTRING_SYMBOL, { "SUBSTR" }},
    { MySQLLexer::VARCHAR_SYMBOL, { "VARCHARACTER" }},
    { MySQLLexer::VARIANCE_SYMBOL, { "VAR_POP" }},
    { MySQLLexer::TINYINT_SYMBOL, { "INT1" }},
    { MySQLLexer::SMALLINT_SYMBOL, { "INT2" }},
    { MySQLLexer::MEDIUMINT_SYMBOL, { "INT3" }},
    { MySQLLexer::BIGINT_SYMBOL, { "INT8" }},
    { MySQLLexer::FRAC_SECOND_SYMBOL, { "SQL_TSI_FRAC_SECOND" }},
    { MySQLLexer::SECOND_SYMBOL, { "SQL_TSI_SECOND" }},
    { MySQLLexer::MINUTE_SYMBOL, { "SQL_TSI_MINUTE" }},
    { MySQLLexer::HOUR_SYMBOL, { "SQL_TSI_HOUR" }},
    { MySQLLexer::DAY_SYMBOL, { "SQL_TSI_DAY" }},
    { MySQLLexer::WEEK_SYMBOL, { "SQL_TSI_WEEK" }},
    { MySQLLexer::MONTH_SYMBOL, { "SQL_TSI_MONTH" }},
    { MySQLLexer::QUARTER_SYMBOL, { "SQL_TSI_QUARTER" }},
    { MySQLLexer::YEAR_SYMBOL, { "SQL_TSI_YEAR" }},
  };

  std::vector<std::pair<int, std::string>> result;

  context.collectCandidates(parser, { caretOffset, caretLine + 1});

  Scanner scanner(dynamic_cast<BufferedTokenStream *>(parser->getTokenStream()));

  MySQLQueryType queryType = QtUnknown;
  MySQLLexer *lexer = dynamic_cast<MySQLLexer *>(parser->getTokenStream()->getTokenSource());
  if (lexer != nullptr)
    queryType = lexer->determineQueryType();

  dfa::Vocabulary const& vocabulary = parser->getVocabulary();

  // Move to caret position and store that on the scanner stack.
  scanner.advanceToPosition(caretLine + 1, caretOffset);
  scanner.push();

  for (auto &candidate : context.completionCandidates.tokens) {
    std::string entry = vocabulary.getDisplayName(candidate.first);
    if (entry.rfind("_SYMBOL") != std::string::npos)
      entry.resize(entry.size() - 7);
    else
      entry = base::unquote(entry);

    size_t list = 0; // The list where we place the entry in.
    if (!candidate.second.empty()) {
      // A function call?
      if (candidate.second[0] == MySQLLexer::OPEN_PAR_SYMBOL) {
        list = 1;
      } else {
        for (size_t token : candidate.second) {
          std::string subEntry = vocabulary.getDisplayName(token);
          if (subEntry.rfind("_SYMBOL") != std::string::npos)
            subEntry.resize(subEntry.size() - 7);
          else
            subEntry = base::unquote(subEntry);
          entry += " " + subEntry;
        }
      }
    }

    switch (list) {
      case 1:
        runtimeFunctionEntries.insert({ AC_FUNCTION_IMAGE, base::tolower(entry) + "()" });
        break;

      default:
        if (!uppercaseKeywords)
          entry = base::tolower(entry);

        keywordEntries.insert({ AC_KEYWORD_IMAGE, entry });
    }
  }

  for (auto &candidate : context.completionCandidates.rules) {
    // Restore the scanner position to the caret position and store that value again for the next round.
    scanner.pop();
    scanner.push();
    
    switch (candidate.first) {
      case MySQLParser::RuleRuntimeFunctionCall: {
        logDebug3("Adding runtime function names\n");

        std::vector<std::string> functions = base::split_by_set(functionNames, " \t\n");
        for (auto &function : functions)
          runtimeFunctionEntries.insert({ AC_FUNCTION_IMAGE, function + "()" });
        break;
      }

      case MySQLParser::RuleFunctionRef:
      case MySQLParser::RuleFunctionCall: {
        std::string qualifier;
        ObjectFlags flags = determineQualifier(scanner, lexer, caretOffset, qualifier);

        if (qualifier.empty()) {
          logDebug3("Adding user defined function names from cache\n");

          std::vector<std::string> functions = cache->getMatchingUdfNames("");
          for (auto &function : functions)
            runtimeFunctionEntries.insert({ AC_FUNCTION_IMAGE, function + "()" });
        }

        logDebug3("Adding function names from cache\n");

        if ((flags & ShowFirst) != 0)
          insertSchemas(cache, schemaEntries, "");

        if ((flags & ShowSecond) != 0)
        {
          if (qualifier.empty())
            qualifier = defaultSchema;

          std::vector<std::string> functions = cache->getMatchingFunctionNames(qualifier, "");
          for (auto &function : functions)
            functionEntries.insert({ AC_ROUTINE_IMAGE, function });
        }

        break;
      }

      case MySQLParser::RuleEngineRef: {
        logDebug3("Adding engine names\n");

        std::vector<std::string> engines = cache->getMatchingEngines("");
        for (auto &engine : engines)
          engineEntries.insert({ AC_ENGINE_IMAGE, engine });
        break;
      }

      case MySQLParser::RuleSchemaRef: {
        logDebug3("Adding schema names from cache\n");

        insertSchemas(cache, schemaEntries, "");
        break;
      }

      case MySQLParser::RuleProcedureRef: {
        logDebug3("Adding procedure names from cache\n");

        std::string qualifier;
        ObjectFlags flags = determineQualifier(scanner, lexer, caretOffset, qualifier);

        if ((flags & ShowFirst) != 0)
          insertSchemas(cache, schemaEntries, "");

        if ((flags & ShowSecond) != 0) {
          if (qualifier.empty())
            qualifier = defaultSchema;

          std::vector<std::string> procedures = cache->getMatchingProcedureNames(qualifier, "");
          for (auto &procedure : procedures)
            procedureEntries.insert({ AC_ROUTINE_IMAGE, procedure });
        }
        break;
      }

      case MySQLParser::RuleTableRefWithWildcard: {
        // A special form of table references (id.id.*) used only in multi-table delete.
        // Handling is similar as for column references (just that we have table/view objects instead of column refs).
        logDebug3("Adding table + view names from cache\n");

        std::string schema, table;
        ObjectFlags flags = determineSchemaTableQualifier(scanner, lexer, schema, table);
        if ((flags & ShowSchemas) != 0)
          insertSchemas(cache, schemaEntries, "");

        std::set<std::string> schemas;
        schemas.insert(schema.empty() ? defaultSchema : schema);
        if ((flags & ShowTables) != 0) {
          insertTables(cache, tableEntries, schemas, "");
          insertViews(cache, viewEntries, schemas, "");
        }
        break;
      }

      case MySQLParser::RuleTableRef:
      case MySQLParser::RuleFilterTableRef:
      case MySQLParser::RuleTableRefNoDb: {
        logDebug3("Adding table + view names from cache\n");

        // Tables refs - also allow view refs.
        std::string qualifier;
        ObjectFlags flags = determineQualifier(scanner, lexer, caretOffset, qualifier);

        if ((flags & ShowFirst) != 0)
          insertSchemas(cache, schemaEntries, "");

        if ((flags & ShowSecond) != 0) {
          std::set<std::string> schemas;
          schemas.insert(qualifier.empty() ? defaultSchema : qualifier);

          insertTables(cache, tableEntries, schemas, "");
          insertViews(cache, viewEntries, schemas, "");
        }
        break;
      }

      case MySQLParser::RuleTableWild:
      case MySQLParser::RuleColumnRef:
      case MySQLParser::RuleColumnInternalRef: {
        logDebug3("Adding column names from cache\n");

        // Try limiting what to show to the smallest set possible.
        // If we have table references show columns only from them.
        // Show columns from the default schema only if there are no references.
        std::string schema, table;
        ObjectFlags flags = determineSchemaTableQualifier(scanner, lexer, schema, table);
        if ((flags & ShowSchemas) != 0)
          insertSchemas(cache, schemaEntries, "");

        // If a schema is given then list only tables + columns from that schema.
        // If no schema is given but we have table references use the schemas from them.
        // Otherwise use the default schema.
        // TODO: case sensitivity.
        std::set<std::string> schemas;

        if (!schema.empty())
          schemas.insert(schema);
        else
          if (!context.references.empty()) {
            for (size_t i = 0; i < context.references.size(); ++ i)
            {
              if (!context.references[i].schema.empty())
                schemas.insert(context.references[i].schema);
            }
          }

        if (schemas.empty())
          schemas.insert(defaultSchema);

        if ((flags & ShowTables) != 0) {
          insertTables(cache, tableEntries, schemas, "");
          if (candidate.first == MySQLParser::RuleColumnRef)
          {
            // Insert also views.
            insertViews(cache, viewEntries, schemas, "");

            // Insert also tables from our references list.
            for (auto &reference : context.references) {
              // If no schema was specified then allow also tables without a given schema. Otherwise
              // the reference's schema must match any of the specified schemas (which include those from the ref list).
              if ((schema.empty() && reference.schema.empty()) || (schemas.count(reference.schema) > 0))
                tableEntries.insert({ AC_TABLE_IMAGE, reference.alias.empty() ? reference.table : reference.alias });
            }
          }
        }

        if ((flags & ShowColumns) != 0) {
          if (schema == table) // Schema and table are equal if it's not clear if we see a schema or table qualfier.
            schemas.insert(defaultSchema);

          // For the columns we use a similar approach like for the schemas.
          // If a table is given, list only columns from this (use the set of schemas from above).
          // If not and we have table references then show columns from them.
          // Otherwise show no columns.
          std::set<std::string> tables;
          if (!table.empty()) {
            tables.insert(table);

            // Could be an alias.
            for (size_t i = 0; i < context.references.size(); ++ i)
              if (base::same_string(table, context.references[i].alias)) {
                tables.insert(context.references[i].table);
                break;
              }
          }
          else
            if (!context.references.empty() && candidate.first == MySQLParser::RuleColumnRef) {
              for (size_t i = 0; i < context.references.size(); ++ i)
                tables.insert(context.references[i].table);
            }

          if (!tables.empty())
            insertColumns(cache, columnEntries, schemas, tables, "");

          // Special deal here: triggers. Show columns for the "new" and "old" qualifiers too.
          // Use the first reference in the list, which is the table to which this trigger belongs (there can be more
          // if the trigger body references other tables).
          if (queryType == QtCreateTrigger && !context.references.empty()
              && (base::same_string(table, "old") || base::same_string(table, "new"))) {
            tables.clear();
            tables.insert(context.references[0].table);
            insertColumns(cache, columnEntries, schemas, tables, "");
          }
        }

        break;
      }

      case MySQLParser::RuleTriggerRef: {
        // Trigger references only consist of a table name and the trigger name.
        // However we have to make sure to show only triggers from the current schema.
        logDebug3("Adding trigger names from cache\n");

        std::string qualifier;
        ObjectFlags flags = determineQualifier(scanner, lexer, caretOffset, qualifier);

        std::set<std::string> schemas;
        schemas.insert(defaultSchema);

        if ((flags & ShowFirst) != 0)
          insertTables(cache, schemaEntries, schemas, "");

        if ((flags & ShowSecond) != 0) {
          std::vector<std::string> triggers = cache->getMatchingTriggerNames(defaultSchema, qualifier, "");
          for (auto &trigger : triggers)
            triggerEntries.insert({ AC_TRIGGER_IMAGE, trigger });
        }
        break;
      }

      case MySQLParser::RuleViewRef: {
        logDebug3("Adding view names from cache\n");

        // View refs only (no table references), e.g. like in DROP VIEW ...
        std::string qualifier;
        ObjectFlags flags = determineQualifier(scanner, lexer, caretOffset, qualifier);

        if ((flags & ShowFirst) != 0)
          insertSchemas(cache, schemaEntries, "");

        if ((flags & ShowSecond) != 0) {
          std::set<std::string> schemas;
          schemas.insert(qualifier.empty() ? defaultSchema : qualifier);
          insertViews(cache, viewEntries, schemas, "");
        }
        break;
      }

      case MySQLParser::RuleLogfileGroupRef: {
        logDebug3("Adding logfile group names from cache\n");

        std::vector<std::string> logfileGroups = cache->getMatchingLogfileGroups("");
        for (auto &logfileGroup : logfileGroups)
          logfileGroupEntries.insert({ AC_LOGFILE_GROUP_IMAGE, logfileGroup });
        break;
      }

      case MySQLParser::RuleTablespaceRef: {
        logDebug3("Adding tablespace names from cache\n");

        std::vector<std::string> tablespaces = cache->getMatchingTablespaces("");
        for (auto &tablespace : tablespaces)
          tablespaceEntries.insert({ AC_TABLESPACE_IMAGE, tablespace });
        break;
      }

      case MySQLParser::RuleUserVariable: {
        logDebug3("Adding user variables\n");

        userVarEntries.insert({ AC_USER_VAR_IMAGE, "<user variable>" });
        break;
      }

      case MySQLParser::RuleLabelRef: {
        logDebug3("Adding label references\n");

        userVarEntries.insert({ AC_USER_VAR_IMAGE, "<block labels>" });
        break;
      }
        
      case MySQLParser::RuleSystemVariable: {
        logDebug3("Adding system variables\n");

        std::vector<std::string> variables = cache->getMatchingVariables("");
        for (auto &variable : variables)
          systemVarEntries.insert({ AC_SYSTEM_VAR_IMAGE, variable });
        break;
      }

      case MySQLParser::RuleCharsetName: {
        logDebug3("Adding charsets\n");

        std::vector<std::string> charsets = cache->getMatchingCharsets("");
        for (auto &charset : charsets)
          charsetEntries.insert({ AC_CHARSET_IMAGE, charset });
        break;
      }

      case MySQLParser::RuleCollationName: {
        logDebug3("Adding collations\n");

        std::vector<std::string> collations = cache->getMatchingCollations("");
        for (auto &collation : collations)
          collationEntries.insert({ AC_COLLATION_IMAGE, collation });
        break;
      }

      case MySQLParser::RuleEventRef: {
        logDebug3("Adding events\n");

        std::string qualifier;
        ObjectFlags flags = determineQualifier(scanner, lexer, caretOffset, qualifier);

        if ((flags & ShowFirst) != 0)
          insertSchemas(cache, schemaEntries, "");

        if ((flags & ShowSecond) != 0)
        {
          if (qualifier.empty())
            qualifier = defaultSchema;

          std::vector<std::string> events = cache->getMatchingEvents(qualifier, "");
          for (auto &event : events)
            eventEntries.insert({ AC_EVENT_IMAGE, event });
        }
        break;
      }
    }
  }

  scanner.pop(); // Clear the scanner stack.

  // Insert the groups "inside out", that is, most likely ones first + most inner first (columns before tables etc).
  std::copy(keywordEntries.begin(), keywordEntries.end(), std::back_inserter(result));
  std::copy(columnEntries.begin(), columnEntries.end(), std::back_inserter(result));
  std::copy(tableEntries.begin(), tableEntries.end(), std::back_inserter(result));
  std::copy(viewEntries.begin(), viewEntries.end(), std::back_inserter(result));
  std::copy(schemaEntries.begin(), schemaEntries.end(), std::back_inserter(result));

  // Everything else is significantly less used.
  // TODO: make this configurable.
  // TODO: show an optimized (small) list of candidates on first invocation, a full list on every following.
  std::copy(functionEntries.begin(), functionEntries.end(), std::back_inserter(result));
  std::copy(procedureEntries.begin(), procedureEntries.end(), std::back_inserter(result));
  std::copy(triggerEntries.begin(), triggerEntries.end(), std::back_inserter(result));
  std::copy(indexEntries.begin(), indexEntries.end(), std::back_inserter(result));
  std::copy(eventEntries.begin(), eventEntries.end(), std::back_inserter(result));
  std::copy(userEntries.begin(), userEntries.end(), std::back_inserter(result));
  std::copy(engineEntries.begin(), engineEntries.end(), std::back_inserter(result));
  std::copy(pluginEntries.begin(), pluginEntries.end(), std::back_inserter(result));
  std::copy(logfileGroupEntries.begin(), logfileGroupEntries.end(), std::back_inserter(result));
  std::copy(tablespaceEntries.begin(), tablespaceEntries.end(), std::back_inserter(result));
  std::copy(charsetEntries.begin(), charsetEntries.end(), std::back_inserter(result));
  std::copy(collationEntries.begin(), collationEntries.end(), std::back_inserter(result));
  std::copy(userVarEntries.begin(), userVarEntries.end(), std::back_inserter(result));
  std::copy(runtimeFunctionEntries.begin(), runtimeFunctionEntries.end(), std::back_inserter(result));
  std::copy(systemVarEntries.begin(), systemVarEntries.end(), std::back_inserter(result));
 
  return result;
}

//--------------------------------------------------------------------------------------------------
