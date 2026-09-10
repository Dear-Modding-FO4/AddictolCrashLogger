#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <dia2.h>
#include <diacreate.h>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace Crash
{
	namespace PDB
	{
		struct SymbolDetails
		{
			std::string text;
			std::string parameters;
			std::string pdbPath;
			std::string issue;
			HRESULT status{ S_FALSE };
			bool hasSourceLine{};
		};

		struct SourceLine
		{
			std::string file;
			DWORD number{};
		};

		struct SourceLines
		{
			std::vector<SourceLine> lines;
			HRESULT status{ S_FALSE };
			bool limitReached{};
		};

		[[nodiscard]] SourceLines read_source_lines(IDiaEnumLineNumbers* a_lines);

		class SymbolResolver
		{
		public:
			SymbolResolver();
			~SymbolResolver();
			SymbolResolver(const SymbolResolver&) = delete;
			SymbolResolver& operator=(const SymbolResolver&) = delete;
			SymbolResolver(SymbolResolver&&) = delete;
			SymbolResolver& operator=(SymbolResolver&&) = delete;

			[[nodiscard]] SymbolDetails resolve(std::string_view a_modulePath, std::uintptr_t a_offset);

		private:
			// DIA sessions are created, used, and released on this resolver's construction thread.
			class Impl;
			std::unique_ptr<Impl> m_impl;
		};

		std::string processSymbol(IDiaSymbol* symbol, IDiaSession* pSession, const DWORD& rva, std::string_view& a_name, uintptr_t& a_offset, std::string& a_result);
		std::string pdb_details(std::string_view a_name, uintptr_t a_offset);
		std::string pdb_function_parameters(std::string_view a_name, uintptr_t a_offset);
		void dump_symbols(bool exe = false);
		void dumpFileSymbols(const std::filesystem::path& path, int& retflag);

		std::string demangle(const std::wstring& mangled); // Existing overload
		std::string demangle(const std::string& mangled);  // Overload for narrow string demangling

		inline constexpr std::string_view sPluginPath = "Data/F4SE/Plugins";
		static HRESULT hr{ -1 };
	}
}
