/**
 * fce_module.cpp — pybind11 bindings module
 *
 * Usage from Python:
 *   import fce
 *   engine = fce.Engine()
 *   engine.initialize()
 *   engine.index_files(["src/main.cpp", "src/foo.hpp"])
 *   result = engine.query("MyClass", bfs_depth=2)
 *   engine.release()
 */

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include "Core/FceEngine.hpp"
#include "Core/QueryEngine.hpp"
#include "Symbol/RelationKind.hpp"
#include "Symbol/DensityConfig.hpp"
#include "Baked/Preprocessor.hpp"

namespace py = pybind11;

PYBIND11_MODULE(fce, m)
{
    m.doc() = "FCE — Flat Code Engine: in-memory code graph for instant symbol lookup";

    // ── RelationKind enum ────────────────────────────────────────────────
    py::enum_<fce::RelationKind>(m, "RelationKind")
        .value("Inherits",   fce::RelationKind::Inherits)
        .value("DerivedBy",  fce::RelationKind::DerivedBy)
        .value("MemberOf",   fce::RelationKind::MemberOf)
        .value("HasMember",  fce::RelationKind::HasMember)
        .value("DefinedIn",  fce::RelationKind::DefinedIn)
        .value("Contains",   fce::RelationKind::Contains)
        .value("Includes",   fce::RelationKind::Includes)
        .value("IncludedBy", fce::RelationKind::IncludedBy)
        .value("Calls",      fce::RelationKind::Calls)
        .value("CalledBy",   fce::RelationKind::CalledBy)
        .value("Returns",    fce::RelationKind::Returns)
        .value("ReturnedBy", fce::RelationKind::ReturnedBy)
        .value("TypedAs",    fce::RelationKind::TypedAs)
        .value("TypeOf",     fce::RelationKind::TypeOf)
        .export_values();

    // ── SymbolKind enum ──────────────────────────────────────────────────
    py::enum_<fce::SymbolKind>(m, "SymbolKind")
        .value("Function",  fce::SymbolKind::Function)
        .value("Class",     fce::SymbolKind::Class)
        .value("Struct",    fce::SymbolKind::Struct)
        .value("Enum",      fce::SymbolKind::Enum)
        .value("Variable",  fce::SymbolKind::Variable)
        .value("Macro",     fce::SymbolKind::Macro)
        .value("Alias",     fce::SymbolKind::Alias)
        .value("Include",   fce::SymbolKind::Include)
        .value("Namespace", fce::SymbolKind::Namespace)
        .value("Concept",   fce::SymbolKind::Concept)
        .value("Interface", fce::SymbolKind::Interface)
        .value("File",      fce::SymbolKind::File)
        .value("Unknown",   fce::SymbolKind::Unknown)
        .export_values();

    // ── SymbolEntry ──────────────────────────────────────────────────────
    py::class_<fce::SymbolEntry>(m, "SymbolEntry")
        .def_readonly("kind",         &fce::SymbolEntry::kind)
        .def_readonly("name",         &fce::SymbolEntry::name)
        .def_readonly("parent_class", &fce::SymbolEntry::parent_class)
        .def_readonly("file_path",    &fce::SymbolEntry::file_path)
        .def_readonly("start_line",   &fce::SymbolEntry::start_line)
        .def_readonly("end_line",     &fce::SymbolEntry::end_line)
        .def_readonly("signature",    &fce::SymbolEntry::signature)
        .def("__repr__", [](const fce::SymbolEntry& e) {
            return "<SymbolEntry " + e.kind + " " + e.name +
                   " @ " + e.file_path + ":" + std::to_string(e.start_line) + ">";
        });

    // ── RelatedSymbol ────────────────────────────────────────────────────
    py::class_<fce::RelatedSymbol>(m, "RelatedSymbol")
        .def_readonly("name",       &fce::RelatedSymbol::name)
        .def_readonly("kind",       &fce::RelatedSymbol::kind)
        .def_readonly("file_path",  &fce::RelatedSymbol::file_path)
        .def_readonly("start_line", &fce::RelatedSymbol::start_line)
        .def_readonly("end_line",   &fce::RelatedSymbol::end_line)
        .def_readonly("signature",  &fce::RelatedSymbol::signature)
        .def_readonly("relation",   &fce::RelatedSymbol::relation)
        .def_readonly("depth",      &fce::RelatedSymbol::depth)
        .def("__repr__", [](const fce::RelatedSymbol& r) {
            return "<RelatedSymbol " + r.kind + " " + r.name +
                   " depth=" + std::to_string(r.depth) + ">";
        });

    // ── QueryResult ──────────────────────────────────────────────────────
    py::class_<fce::QueryResult>(m, "QueryResult")
        .def_readonly("symbols",         &fce::QueryResult::symbols)
        .def_readonly("related",         &fce::QueryResult::related)
        .def_readonly("related_symbols", &fce::QueryResult::related_symbols);

    // ── DensityResult ────────────────────────────────────────────────────
    py::class_<fce::DensityResult>(m, "DensityResult")
        .def_readonly("weighted_score",  &fce::DensityResult::weightedScore)
        .def_readonly("edge_type_count", &fce::DensityResult::edgeTypeCount)
        .def_readonly("sufficient",      &fce::DensityResult::sufficient)
        .def_readonly("expansions_used", &fce::DensityResult::expansionsUsed);

    // ── SmartQueryResult ─────────────────────────────────────────────────
    py::class_<fce::SmartQueryResult>(m, "SmartQueryResult")
        .def_readonly("data",    &fce::SmartQueryResult::data)
        .def_readonly("density", &fce::SmartQueryResult::density);

    // ── SnippetResult ────────────────────────────────────────────────────
    py::class_<fce::SnippetResult>(m, "SnippetResult")
        .def_readonly("file_path",      &fce::SnippetResult::file_path)
        .def_readonly("start_line",     &fce::SnippetResult::start_line)
        .def_readonly("end_line",       &fce::SnippetResult::end_line)
        .def_readonly("code",           &fce::SnippetResult::code)
        .def_readonly("pincer_applied", &fce::SnippetResult::pincer_applied);

    // ── DensityConfig ────────────────────────────────────────────────────
    py::class_<fce::DensityConfig>(m, "DensityConfig")
        .def(py::init<>())
        .def_readwrite("score_threshold", &fce::DensityConfig::scoreThreshold)
        .def_readwrite("min_edge_types",  &fce::DensityConfig::minEdgeTypes)
        .def_readwrite("max_expansions",  &fce::DensityConfig::maxExpansions)
        .def_readwrite("goldilocks_min",  &fce::DensityConfig::goldilocksMin)
        .def_readwrite("goldilocks_max",  &fce::DensityConfig::goldilocksMax)
        .def_static("default_config",     &fce::DensityConfig::Default);

    // ── PreprocessConfig ─────────────────────────────────────────────────
    py::class_<fce::PreprocessConfig>(m, "PreprocessConfig")
        .def(py::init<>())
        .def_readwrite("enabled",       &fce::PreprocessConfig::enabled)
        .def_readwrite("include_paths", &fce::PreprocessConfig::includePaths)
        .def_readwrite("defines",       &fce::PreprocessConfig::defines)
        .def_readwrite("extra_flags",   &fce::PreprocessConfig::extraFlags)
        .def_readwrite("batch_size",    &fce::PreprocessConfig::batchSize);

    // ── Relation filter constants ──────────────────────────────────────────
    m.attr("ALL_RELATIONS")      = fce::kAllRelations;
    m.attr("SEMANTIC_RELATIONS") = fce::kSemanticRelations;
    m.def("relation_bit", &fce::RelationBit, py::arg("kind"),
          "Convert RelationKind to bitmask");

    // ── Engine (FceEngine wrapper) ─────────────────────────────────────────
    py::class_<fce::FceEngine>(m, "Engine")
        .def(py::init<>())

        // Lifecycle
        .def("initialize", &fce::FceEngine::Initialize,
             "Initialize engine (must be the first call)")
        .def("release", &fce::FceEngine::Release,
             "Release engine resources")

        // Indexing
        .def("index_files", &fce::FceEngine::IndexLocalFiles,
             py::arg("file_paths"),
             "Parse file list and build symbol index. Returns number of processed files.")
        .def("reindex_files", &fce::FceEngine::ReindexFiles,
             py::arg("file_paths"),
             "Remove and re-parse specific files (incremental update)")
        .def("remove_files", &fce::FceEngine::RemoveFiles,
             py::arg("file_paths"),
             "Remove symbols/edges for specific files")

        // Preprocessing config
        .def("set_preprocess_config", &fce::FceEngine::SetPreprocessConfig,
             py::arg("config"),
             "C preprocessor config (gcc -E)")

        // Query
        .def("query", &fce::FceEngine::Query,
             py::arg("name"),
             py::arg("bfs_depth") = 2,
             py::arg("filter") = fce::kAllRelations,
             py::arg("reverse") = false,
             "BFS graph traversal by symbol name")
        .def("smart_query", &fce::FceEngine::SmartQuery,
             py::arg("name"),
             py::arg("density_config") = fce::DensityConfig::Default(),
             "Density evaluation + auto-expansion smart query")
        .def("lookup", &fce::FceEngine::Lookup,
             py::arg("name"),
             "Direct lookup by symbol name")
        .def("symbol_count", &fce::FceEngine::SymbolCount,
             "Total number of indexed symbols")

        // Pincer extraction
        .def_static("pincer_extract", &fce::FceEngine::PincerExtract,
             py::arg("file_path"),
             py::arg("start_line"),
             py::arg("end_line"),
             py::arg("target_name"),
             py::arg("radius") = 7,
             "Extract ±radius lines around target call site from file")

        // Introspection (returns JSON strings)
        .def("get_file_tree", &fce::FceEngine::GetFileTree,
             "List of indexed files (JSON)")
        .def("get_file_symbols", &fce::FceEngine::GetFileSymbols,
             py::arg("path"),
             "Symbol list for a specific file (JSON)")
        .def("search_symbols", &fce::FceEngine::SearchSymbols,
             py::arg("prefix"), py::arg("limit") = 20,
             "Search symbols by name prefix/substring (JSON)")

        // context manager support
        .def("__enter__", [](fce::FceEngine& self) -> fce::FceEngine& {
            self.Initialize();
            return self;
        })
        .def("__exit__", [](fce::FceEngine& self, py::object, py::object, py::object) {
            self.Release();
        });
}
