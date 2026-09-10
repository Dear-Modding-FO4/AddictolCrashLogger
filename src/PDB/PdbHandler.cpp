// SPDX-License-Identifier: CC-BY-SA-4.0
// Code from StackOverflow

#include "PdbHandler.h"
#include "Capture/DbgHelpGate.h"
#include "Settings.h"
#include <DbgHelp.h>
#include <algorithm>
#include <array>
#include <atlcomcli.h>
#include <comdef.h>
#include <fmt/format.h>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <vector>

using namespace std::literals;

#undef ERROR

#ifndef E_PDB_USAGE
#	define E_PDB_USAGE HRESULT(0x806D0001L)
#	define E_PDB_OUT_OF_MEMORY HRESULT(0x806D0002L)
#	define E_PDB_FILE_SYSTEM HRESULT(0x806D0003L)
#	define E_PDB_NOT_FOUND HRESULT(0x806D0004L)
#	define E_PDB_INVALID_SIG HRESULT(0x806D0005L)
#	define E_PDB_INVALID_AGE HRESULT(0x806D0006L)
#	define E_PDB_PRECOMP_REQUIRED HRESULT(0x806D0007L)
#	define E_PDB_OUT_OF_TI HRESULT(0x806D0008L)
#	define E_PDB_NOT_IMPLEMENTED HRESULT(0x806D0009L)
#	define E_PDB_V1_PDB HRESULT(0x806D000AL)
#	define E_PDB_FORMAT HRESULT(0x806D000CL)
#	define E_PDB_LIMIT HRESULT(0x806D000DL)
#	define E_PDB_CORRUPT HRESULT(0x806D000EL)
#	define E_PDB_TI16 HRESULT(0x806D000FL)
#	define E_PDB_ACCESS_DENIED HRESULT(0x806D0010L)
#	define E_PDB_ILLEGAL_TYPE_EDIT HRESULT(0x806D0011L)
#	define E_PDB_INVALID_EXECUTABLE HRESULT(0x806D0012L)
#	define E_PDB_DBG_NOT_FOUND HRESULT(0x806D0013L)
#	define E_PDB_NO_DEBUG_INFO HRESULT(0x806D0014L)
#	define E_PDB_INVALID_EXE_TIMESTAMP HRESULT(0x806D0015L)
#	define E_PDB_RESERVED HRESULT(0x806D0016L)
#	define E_PDB_DEBUG_INFO_NOT_IN_PDB HRESULT(0x806D0017L)
#	define E_PDB_SYMSRV_BAD_CACHE_PATH HRESULT(0x806D0018L)
#	define E_PDB_SYMSRV_CACHE_FULL HRESULT(0x806D0019L)
#	define E_PDB_MAX HRESULT(0x806D001AL)
#endif

namespace Crash
{
	namespace PDB
	{
		std::string ConvertWCSToMBS(const wchar_t* pstr, long wslen)
		{
			if (wslen == 0)
				return {};
			if (!pstr || wslen < 0)
				throw std::invalid_argument("Invalid wide symbol string");
			const auto size = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, pstr, wslen, nullptr, 0, nullptr, nullptr);
			if (size == 0)
				throw std::system_error(GetLastError(), std::system_category(), "Symbol text conversion");
			std::string result(static_cast<std::size_t>(size), '\0');
			if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, pstr, wslen, result.data(), size, nullptr, nullptr) != size)
				throw std::system_error(GetLastError(), std::system_category(), "Symbol text conversion");
			return result;
		}

		std::string ConvertBSTRToMBS(BSTR bstr)
		{
			const auto size = ::SysStringLen(bstr);
			if (size > static_cast<unsigned long>((std::numeric_limits<long>::max)()))
				throw std::length_error("Symbol text exceeds conversion limit");
			return ConvertWCSToMBS(bstr, static_cast<long>(size));
		}

		std::wstring utf8_to_utf16(const std::string& utf8)
		{
			if (utf8.empty())
				return {};
			if (utf8.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)()))
				throw std::length_error("Symbol path exceeds conversion limit");
			const auto length = static_cast<int>(utf8.size());
			const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), length, nullptr, 0);
			if (size == 0)
				throw std::system_error(GetLastError(), std::system_category(), "Symbol path conversion");
			std::wstring result(static_cast<std::size_t>(size), L'\0');
			if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), length, result.data(), size) != size)
				throw std::system_error(GetLastError(), std::system_category(), "Symbol path conversion");
			return result;
		}

		std::string utf16_to_utf8(const std::wstring& utf16)
		{
			if (utf16.size() > static_cast<std::size_t>((std::numeric_limits<long>::max)()))
				throw std::length_error("Symbol text exceeds conversion limit");
			return ConvertWCSToMBS(utf16.data(), static_cast<long>(utf16.size()));
		}

		[[nodiscard]] std::string demangle(const std::wstring& mangled)
		{
			// Early return for non-mangled names (Microsoft mangled names start with '?')
			if (mangled.empty() || mangled[0] != L'?')
				return utf16_to_utf8(mangled);

			auto lock = Capture::DbgHelpGate::try_lock();
			if (!lock.owns_lock())
				return utf16_to_utf8(mangled);

			// Use a larger buffer for complex names
			std::array<wchar_t, 0x2000> buffer{ L'\0' };

			const auto length = UnDecorateSymbolNameW(
				mangled.c_str(),
				buffer.data(),
				static_cast<DWORD>(buffer.size()),
				UNDNAME_COMPLETE |                    // Full demangling
				UNDNAME_NO_LEADING_UNDERSCORES |  // Remove leading underscores
				UNDNAME_NO_MS_KEYWORDS |          // Remove MS-specific keywords
				//UNDNAME_NO_FUNCTION_RETURNS |     // Don't show function return types
				UNDNAME_NO_ALLOCATION_MODEL |     // Remove allocation model
				UNDNAME_NO_ALLOCATION_LANGUAGE |  // Remove allocation language
				UNDNAME_NO_THISTYPE |             // Don't show 'this' type
				UNDNAME_NO_ACCESS_SPECIFIERS |    // Remove public/private/protected
				UNDNAME_NO_THROW_SIGNATURES |     // Remove throw specifications
				UNDNAME_NO_RETURN_UDT_MODEL |     // Remove return UDT model
				static_cast<DWORD>(0x8000));      // Disable enum/class/struct/union prefix

			// Check if demangling succeeded
			if (length == 0 || buffer[0] == L'\0')
				return utf16_to_utf8(mangled);  // Failed, return original

			// Ensure proper null termination
			if (length < buffer.size())
				buffer[length] = L'\0';

			std::wstring demangled{ buffer.data() };

			// Trim whitespace
			demangled.erase(0, demangled.find_first_not_of(L" \t\r\n"));
			demangled.erase(demangled.find_last_not_of(L" \t\r\n") + 1);

			// Check for failed demangling indicators
			if (demangled.empty() ||
				demangled == L"<unknown>" ||
				demangled == L"UNKNOWN" ||
				demangled.starts_with(L"??"))
			{
				return utf16_to_utf8(mangled);
			}

			// For crash analysis, show both demangled and original
			return utf16_to_utf8(demangled) + " [" + utf16_to_utf8(mangled) + "]";
		}

		// Overload for std::string (narrow string) - now handles RTTI and MSVC symbols
		[[nodiscard]] std::string demangle(const std::string& mangled)
		{
			if (mangled.empty())
				return mangled;
			if (mangled[0] == '.')
			{
				// RTTI type descriptor: skip the leading dot
				auto lock = Capture::DbgHelpGate::try_lock();
				if (!lock.owns_lock())
					return mangled;
				std::array<char, 0x1000> buf{ '\0' };
				// Use UNDNAME_NAME_ONLY to get just the type name
				const auto len = UnDecorateSymbolName(
					mangled.data() + 1,  // skip leading '.'
					buf.data(),
					static_cast<std::uint32_t>(buf.size()),
					UNDNAME_NAME_ONLY | UNDNAME_NO_ARGUMENTS | static_cast<std::uint32_t>(0x8000));
				if (len != 0)
				{
					std::string name{ buf.data(), len };
					// Clean up whitespace
					name.erase(0, name.find_first_not_of(" \t\r\n"));
					name.erase(name.find_last_not_of(" \t\r\n") + 1);
					return name;
				}
				else
				{
					// Fallback: strip .?AV...@@ to class name
					auto start = mangled.find('A');
					auto end = mangled.rfind("@@");
					if (start != std::string::npos && end != std::string::npos && end > start + 1) {
						return mangled.substr(start + 1, end - start - 1);
					}
					return mangled;
				}
			}
			else if (mangled[0] == '?')
			{
				// MSVC symbol name
				auto lock = Capture::DbgHelpGate::try_lock();
				if (!lock.owns_lock())
					return mangled;
				std::array<char, 0x2000> buffer{ '\0' };
				const auto length = UnDecorateSymbolName(
					mangled.c_str(),
					buffer.data(),
					static_cast<DWORD>(buffer.size()),
					UNDNAME_COMPLETE |
					UNDNAME_NO_LEADING_UNDERSCORES |
					UNDNAME_NO_MS_KEYWORDS |
					UNDNAME_NO_ALLOCATION_MODEL |
					UNDNAME_NO_ALLOCATION_LANGUAGE |
					UNDNAME_NO_THISTYPE |
					UNDNAME_NO_ACCESS_SPECIFIERS |
					UNDNAME_NO_THROW_SIGNATURES |
					UNDNAME_NO_RETURN_UDT_MODEL |
					static_cast<DWORD>(0x8000));

				if (length == 0 || buffer[0] == '\0')
					return mangled;  // Failed, return original

				if (length < buffer.size())
					buffer[length] = '\0';

				std::string demangled{ buffer.data() };
				demangled.erase(0, demangled.find_first_not_of(" \t\r\n"));
				demangled.erase(demangled.find_last_not_of(" \t\r\n") + 1);
				if (demangled.empty() ||
					demangled == "<unknown>" ||
					demangled == "UNKNOWN" ||
					demangled.starts_with("??"))
				{
					return mangled;
				}
				return demangled + " [" + mangled + "]";
			}
			else
				return mangled;
		}

		// Helper for BSTR to std::wstring
		[[nodiscard]] std::wstring bstr_to_wstring(BSTR bstr)
		{
			if (!bstr)
				return std::wstring();
			return std::wstring(bstr, SysStringLen(bstr));
		}

		SourceLines read_source_lines(IDiaEnumLineNumbers* a_lines)
		{
			SourceLines result;
			if (!a_lines)
				return result;
			constexpr std::size_t maximumLines = 5;
			for (std::size_t i = 0; i < maximumLines; ++i)
			{
				CComPtr<IDiaLineNumber> line;
				ULONG fetched{};
				const auto status = a_lines->Next(1, &line, &fetched);
				if (FAILED(status))
				{
					result.status = status;
					return result;
				}
				if (fetched != 1 || !line)
				{
					if (status == S_OK || fetched != 0)
						result.status = E_UNEXPECTED;
					return result;
				}
				SourceLine source;
				CComPtr<IDiaSourceFile> file;
				if (line->get_sourceFile(&file) == S_OK && file)
				{
					CComBSTR name;
					if (file->get_fileName(&name) == S_OK && name)
						source.file = ConvertBSTRToMBS(name);
				}
				if (line->get_lineNumber(&source.number) != S_OK)
					source.number = 0;
				if (!source.file.empty() || source.number != 0)
				{
					result.lines.push_back(std::move(source));
					result.status = S_OK;
				}
				if (status == S_FALSE)
					return result;
			}
			result.limitReached = true;
			return result;
		}

		namespace
		{
			SymbolDetails describe_symbol(IDiaSymbol* a_symbol, IDiaSession* a_session, DWORD a_rva)
			{
				SymbolDetails result;
				if (!a_symbol)
				{
					result.status = E_POINTER;
					result.issue = "No DIA symbol";
					return result;
				}
				CComBSTR name;
				result.status = a_symbol->get_name(&name);
				if (result.status != S_OK || !name || name.Length() == 0)
				{
					if (result.status == S_OK)
						result.status = S_FALSE;
					result.issue = "Symbol name unavailable";
					return result;
				}
				result.text = demangle(bstr_to_wstring(name));
				DWORD symbolRva{};
				if (a_symbol->get_relativeVirtualAddress(&symbolRva) == S_OK && a_rva >= symbolRva && a_rva != symbolRva)
					result.text += fmt::format("+0x{:X}", a_rva - symbolRva);
				if (!a_session)
				{
					result.issue = "Source lines unavailable: no DIA session";
					return result;
				}
				CComPtr<IDiaEnumLineNumbers> enumerator;
				const auto lineStatus = a_session->findLinesByRVA(a_rva, 1, &enumerator);
				if (lineStatus != S_OK || !enumerator)
				{
					result.issue = fmt::format("Source lines unavailable (0x{:08X})", static_cast<std::uint32_t>(lineStatus));
					return result;
				}
				const auto sources = read_source_lines(enumerator);
				for (const auto& source : sources.lines)
				{
					result.text += source.file.empty() ? " at <source unavailable>" : " at " + source.file;
					if (source.number != 0)
					{
						result.text += ":" + std::to_string(source.number);
						result.hasSourceLine = true;
					}
				}
				if (sources.status != S_OK)
					result.issue = fmt::format("Source lines unavailable or partial (0x{:08X})", static_cast<std::uint32_t>(sources.status));
				else if (sources.limitReached)
					result.issue = "Source-line limit reached";
				return result;
			}
		}

		std::string processSymbol(IDiaSymbol* a_symbol, IDiaSession* a_session, const DWORD& a_rva, std::string_view&, uintptr_t&, std::string& a_result)
		{
			DWORD rva = a_rva;
			if (rva == 0 && a_symbol)
				(void)a_symbol->get_relativeVirtualAddress(&rva);
			a_result = describe_symbol(a_symbol, a_session, rva).text;
			return a_result;
		}

		std::string print_hr_failure(HRESULT hr)
		{
			std::string errMsg;
			switch (hr)
			{
				// PDB-specific error codes
			case E_PDB_USAGE:
				errMsg = "Invalid PDB usage";
				break;
			case E_PDB_OUT_OF_MEMORY:
				errMsg = "Out of memory during PDB operation";
				break;
			case E_PDB_FILE_SYSTEM:
				errMsg = "File system error accessing PDB";
				break;
			case E_PDB_NOT_FOUND:
				errMsg = "PDB file not found";
				break;
			case E_PDB_INVALID_SIG:
				errMsg = "PDB signature mismatch";
				break;
			case E_PDB_INVALID_AGE:
				errMsg = "PDB age mismatch";
				break;
			case E_PDB_PRECOMP_REQUIRED:
				errMsg = "Precompiled header required";
				break;
			case E_PDB_OUT_OF_TI:
				errMsg = "Out of type indices";
				break;
			case E_PDB_NOT_IMPLEMENTED:
				errMsg = "PDB feature not implemented";
				break;
			case E_PDB_V1_PDB:
				errMsg = "Unsupported PDB v1.0 format";
				break;
			case E_PDB_FORMAT:
				errMsg = "Invalid PDB format";
				break;
			case E_PDB_LIMIT:
				errMsg = "PDB internal limit exceeded";
				break;
			case E_PDB_CORRUPT:
				errMsg = "PDB file is corrupted";
				break;
			case E_PDB_TI16:
				errMsg = "PDB 16-bit type index not supported";
				break;
			case E_PDB_ACCESS_DENIED:
				errMsg = "Access denied to PDB file";
				break;
			case E_PDB_ILLEGAL_TYPE_EDIT:
				errMsg = "Illegal type edit in PDB";
				break;
			case E_PDB_INVALID_EXECUTABLE:
				errMsg = "Invalid executable format for PDB";
				break;
			case E_PDB_DBG_NOT_FOUND:
				errMsg = "DBG file not found";
				break;
			case E_PDB_NO_DEBUG_INFO:
				errMsg = "No debug information available";
				break;
			case E_PDB_INVALID_EXE_TIMESTAMP:
				errMsg = "Executable timestamp mismatch";
				break;
			case E_PDB_RESERVED:
				errMsg = "Reserved PDB error";
				break;
			case E_PDB_DEBUG_INFO_NOT_IN_PDB:
				errMsg = "Debug info not in PDB format";
				break;
			case E_PDB_SYMSRV_BAD_CACHE_PATH:
				errMsg = "Bad symbol server cache path";
				break;
			case E_PDB_SYMSRV_CACHE_FULL:
				errMsg = "Symbol server cache full";
				break;
			case E_PDB_MAX:
				errMsg = "Maximum PDB error reached";
				break;
				// Common HRESULT codes
			case E_INVALIDARG:
				errMsg = "Invalid argument passed to PDB function";
				break;
			case E_OUTOFMEMORY:
				errMsg = "Out of memory";
				break;
			case E_FAIL:
				errMsg = "Unspecified PDB failure";
				break;
			case E_NOTIMPL:
				errMsg = "PDB function not implemented";
				break;
			case E_NOINTERFACE:
				errMsg = "PDB interface not supported";
				break;
			case E_ACCESSDENIED:
				errMsg = "Access denied to PDB resources";
				break;
			default:
				errMsg = fmt::format("HRESULT 0x{:08X}", static_cast<std::uint32_t>(hr));
				break;
			}
			return errMsg;
		}

		namespace
		{
			[[nodiscard]] std::string base_type_to_string(DWORD baseType, ULONGLONG length)
			{
				switch (baseType)
				{
				case btVoid:
					return "void";
				case btBool:
					return "bool";
				case btChar:
					return "char";
				case btWChar:
					return "wchar_t";
				case btInt:
					switch (length) {
					case 1:
						return "int8_t";
					case 2:
						return "int16_t";
					case 4:
						return "int32_t";
					case 8:
						return "int64_t";
					default:
						return "int";
					}
				case btUInt:
					switch (length)
					{
					case 1:
						return "uint8_t";
					case 2:
						return "uint16_t";
					case 4:
						return "uint32_t";
					case 8:
						return "uint64_t";
					default:
						return "unsigned int";
					}
				case btFloat:
					return length == 8 ? "double" : "float";
				case btLong:
					return "long";
				case btULong:
					return "unsigned long";
				default:
					return "<unknown>";
				}
			}

			[[nodiscard]] std::string get_symbol_name(IDiaSymbol* symbol)
			{
				CComBSTR name;
				if (symbol->get_name(&name) == S_OK && name)
				{
					const auto converted = ConvertBSTRToMBS(name);
					return demangle(converted);
				}
				return "";
			}

			[[nodiscard]] std::string get_type_name(IDiaSymbol* type, unsigned a_depth = 0)
			{
				if (!type)
					return "<unknown>";
				if (a_depth >= 16)
					return "<type depth limit>";

				DWORD symTag = 0;
				if (FAILED(type->get_symTag(&symTag)))
					return "<unknown>";

				switch (symTag)
				{
				case SymTagPointerType:
				{
					CComPtr<IDiaSymbol> pointee;
					type->get_type(&pointee);
					auto name = get_type_name(pointee, a_depth + 1);
					BOOL isConst = FALSE;
					type->get_constType(&isConst);
					if (isConst && !name.starts_with("const "))
						name = "const " + name;

					return name + "*";
				}
				case SymTagBaseType:
				{
					DWORD baseType = 0;
					ULONGLONG length = 0;
					type->get_baseType(&baseType);
					type->get_length(&length);
					return base_type_to_string(baseType, length);
				}
				case SymTagEnum:
				case SymTagUDT:
					return get_symbol_name(type);
				case SymTagArrayType:
				{
					CComPtr<IDiaSymbol> element;
					type->get_type(&element);
					DWORD count = 0;
					type->get_count(&count);
					return fmt::format("{}[{}]", get_type_name(element, a_depth + 1), count);
				}
				case SymTagFunctionType:
					return "function";
				default:
					return get_symbol_name(type);
				}
			}
		}

		// Captures the actual PDB path DIA opens
		class DiaLoadLogger : public IDiaLoadCallback2
		{
		public:
			std::wstring openedPdb;

			ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
			ULONG STDMETHODCALLTYPE Release() override { return 1; }
			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
			{
				if (!ppv)
					return E_INVALIDARG;
				
				if (riid == __uuidof(IUnknown) || riid == __uuidof(IDiaLoadCallback) || riid == __uuidof(IDiaLoadCallback2))
				{
					*ppv = static_cast<IDiaLoadCallback2*>(this);
					return S_OK;
				}

				*ppv = nullptr;
				return E_NOINTERFACE;
			}

			HRESULT STDMETHODCALLTYPE NotifyDebugDir(BOOL, DWORD, BYTE*) override { return S_OK; }
			HRESULT STDMETHODCALLTYPE NotifyOpenDBG(LPCOLESTR, HRESULT) override { return S_OK; }
			HRESULT STDMETHODCALLTYPE NotifyOpenPDB(LPCOLESTR a_pdbPath, HRESULT a_resultCode) override
			{
				if (SUCCEEDED(a_resultCode) && a_pdbPath)
					openedPdb = a_pdbPath;
					
				return S_OK;
			}
			HRESULT STDMETHODCALLTYPE RestrictRegistryAccess() override { return S_OK; }
			HRESULT STDMETHODCALLTYPE RestrictSymbolServerAccess() override { return S_OK; }
			HRESULT STDMETHODCALLTYPE RestrictOriginalPathAccess() override { return S_OK; }
			HRESULT STDMETHODCALLTYPE RestrictReferencePathAccess() override { return S_OK; }
			HRESULT STDMETHODCALLTYPE RestrictDBGAccess() override { return S_OK; }
			HRESULT STDMETHODCALLTYPE RestrictSystemRootAccess() override { return S_OK; }
		};

		// Helper struct to encapsulate PDB session setup
		struct PdbSession
		{
			CComPtr<IDiaDataSource> pSource;
			CComPtr<IDiaSession> pSession;
			CComPtr<IDiaSymbol> globalSymbol;
			bool com_initialized_here = false;
			HRESULT status{ S_FALSE };
			std::string issue;
			std::string pdbPath;

			~PdbSession()
			{
				globalSymbol.Release();
				pSession.Release();
				pSource.Release();
				if (com_initialized_here)
					CoUninitialize();
			}

			bool fail(HRESULT a_status, std::string_view a_operation)
			{
				status = a_status;
				issue = std::string(a_operation) + ": " + print_hr_failure(a_status);
				return false;
			}

			// Open a PDB session for the given module
			bool open(std::string_view a_name)
			{
				std::filesystem::path dllPath{ utf8_to_utf16(std::string(a_name)) };
				if (!dllPath.has_parent_path())
					dllPath = std::filesystem::path(sPluginPath) / dllPath;

				auto result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
				if (FAILED(result) && result != RPC_E_CHANGED_MODE)
					return fail(result, "COM initialization");
				com_initialized_here = SUCCEEDED(result);

				const auto diaPath = std::filesystem::absolute(
					std::filesystem::path(sPluginPath) / L"msdia140.dll");
				result = NoRegCoCreate(diaPath.c_str(), CLSID_DiaSource, __uuidof(IDiaDataSource), reinterpret_cast<void**>(&pSource));
				if (FAILED(result))
				{
					pSource.Release();
					result = CoCreateInstance(CLSID_DiaSource, nullptr, CLSCTX_INPROC_SERVER,
						__uuidof(IDiaDataSource), reinterpret_cast<void**>(&pSource));
					if (FAILED(result))
						return fail(result, "DIA provider unavailable");
				}
				if (!pSource)
					return fail(E_UNEXPECTED, "DIA provider returned no data source");
				const auto symcache = Settings::sSymcacheDirectory.GetValue();
				std::vector<std::wstring> searchPaths{ std::filesystem::path(sPluginPath).wstring() };
				std::error_code error;
				const std::filesystem::path cachePath{ utf8_to_utf16(symcache) };
				if (!cachePath.empty() && std::filesystem::is_directory(cachePath, error) && !error)
				{
					searchPaths.push_back(L"cache*" + cachePath.wstring());
				}

				DiaLoadLogger loadLogger;
				for (const auto& path : searchPaths)
				{
					result = pSource->loadDataForExe(dllPath.c_str(), path.c_str(), &loadLogger);
					if (SUCCEEDED(result))
						break;
				}
				if (FAILED(result))
					return fail(result, "PDB load");
				pdbPath = utf16_to_utf8(loadLogger.openedPdb);
				result = pSource->openSession(&pSession);
				if (FAILED(result) || !pSession)
					return fail(FAILED(result) ? result : E_UNEXPECTED, "DIA session");
				status = S_OK;
				return true;
			}
		};

		namespace
		{
			void describe_parameters(IDiaSymbol* a_function, SymbolDetails& a_result)
			{
				if (!a_function)
					return;
				CComPtr<IDiaEnumSymbols> symbols;
				const auto status = a_function->findChildren(SymTagData, nullptr, nsNone, &symbols);
				if (FAILED(status) || !symbols)
				{
					if (FAILED(status))
						a_result.issue += fmt::format(" Parameters unavailable (0x{:08X})", static_cast<std::uint32_t>(status));
					return;
				}
				std::size_t parameters{};
				for (std::size_t examined = 0; examined < 256; ++examined)
				{
					CComPtr<IDiaSymbol> child;
					ULONG fetched{};
					const auto next = symbols->Next(1, &child, &fetched);
					if (FAILED(next))
					{
						a_result.issue += fmt::format(" Parameter enumeration failed (0x{:08X})", static_cast<std::uint32_t>(next));
						return;
					}
					if (fetched != 1 || !child)
						return;
					DWORD kind{};
					if (child->get_dataKind(&kind) != S_OK || kind != DataIsParam)
						continue;
					if (parameters == 8)
					{
						a_result.parameters += ", ...";
						a_result.issue += " Parameter limit reached";
						return;
					}
					CComBSTR name;
					CComPtr<IDiaSymbol> type;
					(void)child->get_type(&type);
					const auto paramName = child->get_name(&name) == S_OK && name ? ConvertBSTRToMBS(name) : std::string{};
					if (parameters++ != 0)
						a_result.parameters += ", ";
					a_result.parameters += paramName.empty() ? get_type_name(type) : paramName + ": " + get_type_name(type);
				}
				a_result.issue += " Parameter enumeration budget reached";
			}
		}

		class SymbolResolver::Impl
		{
		public:
			DWORD threadId{ GetCurrentThreadId() };
			std::mutex mutex;
			std::unordered_map<std::string, std::unique_ptr<PdbSession>> sessions;
		};

		SymbolResolver::SymbolResolver() : m_impl(std::make_unique<Impl>()) {}
		SymbolResolver::~SymbolResolver() = default;

		SymbolDetails SymbolResolver::resolve(std::string_view a_modulePath, std::uintptr_t a_offset)
		{
			SymbolDetails result;
			if (m_impl->threadId != GetCurrentThreadId())
			{
				result.status = RPC_E_WRONG_THREAD;
				result.issue = "Symbol resolver belongs to another COM thread";
				return result;
			}
			std::unique_lock lock{ m_impl->mutex, std::try_to_lock };
			if (!lock.owns_lock())
			{
				result.status = HRESULT_FROM_WIN32(ERROR_BUSY);
				result.issue = "Symbol resolver busy";
				return result;
			}
			if (a_modulePath.empty() || a_modulePath.find('\0') != std::string_view::npos ||
				a_offset > (std::numeric_limits<DWORD>::max)())
			{
				result.status = E_INVALIDARG;
				result.issue = "Invalid module path or RVA";
				return result;
			}
			auto found = m_impl->sessions.find(std::string(a_modulePath));
			if (found == m_impl->sessions.end())
			{
				if (m_impl->sessions.size() >= 64)
				{
					result.status = HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_QUOTA);
					result.issue = "Per-report symbol session limit reached";
					return result;
				}
				auto session = std::make_unique<PdbSession>();
				(void)session->open(a_modulePath);
				found = m_impl->sessions.emplace(std::string(a_modulePath), std::move(session)).first;
			}
			auto& session = *found->second;
			result.status = session.status;
			result.issue = session.issue;
			result.pdbPath = session.pdbPath;
			if (session.status != S_OK)
				return result;
			const auto rva = static_cast<DWORD>(a_offset);
			CComPtr<IDiaSymbol> function;
			auto lookupStatus = session.pSession->findSymbolByRVA(rva, SymTagFunction, &function);
			CComPtr<IDiaSymbol> symbol = function;
			if (!symbol)
				lookupStatus = session.pSession->findSymbolByRVA(rva, SymTagPublicSymbol, &symbol);
			if (!symbol)
			{
				result.status = FAILED(lookupStatus) ? lookupStatus : S_FALSE;
				result.issue = FAILED(lookupStatus) ? "Symbol lookup: " + print_hr_failure(lookupStatus) : "No symbol at RVA";
				return result;
			}
			result = describe_symbol(symbol, session.pSession, rva);
			result.pdbPath = session.pdbPath;
			describe_parameters(function, result);
			return result;
		}

		std::string pdb_details(std::string_view a_name, uintptr_t a_offset)
		{
			SymbolResolver resolver;
			const auto result = resolver.resolve(a_name, a_offset);
			return result.text.empty() ? "[symbols unavailable: " + result.issue + "]" : result.text;
		}

		std::string pdb_function_parameters(std::string_view a_name, uintptr_t a_offset)
		{
			SymbolResolver resolver;
			return resolver.resolve(a_name, a_offset).parameters;
		}

		void dump_symbols(bool exe)
		{
			int retflag{};
			if (exe)
			{
				std::array<wchar_t, 32768> path{};
				const auto length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
				if (length == 0 || length >= path.size())
				{
					REX::ERROR("Could not obtain executable path for symbol enumeration");
					return;
				}
				dumpFileSymbols(std::filesystem::path(path.data()), retflag);
			}
			else
			{
				for (const auto& elem : std::filesystem::directory_iterator(Crash::PDB::sPluginPath))
				{
					if (elem.is_regular_file() && _wcsicmp(elem.path().extension().c_str(), L".dll") == 0)
					{
						dumpFileSymbols(elem.path(), retflag);
						if (retflag == 3)
							continue;
					}
				}
			}
		}
		void dumpFileSymbols(const std::filesystem::path& path, int& retflag)
		{
			retflag = 1;
			PdbSession session;
			if (!session.open(utf16_to_utf8(path.wstring())))
			{
				REX::WARN("Symbol enumeration unavailable: {}", session.issue);
				retflag = 3;
				return;
			}
			CComPtr<IDiaEnumSymbolsByAddr> symbols;
			if (FAILED(session.pSession->getSymbolsByAddr(&symbols)) || !symbols)
			{
				REX::WARN("Symbol address enumeration unavailable");
				retflag = 3;
				return;
			}
			CComPtr<IDiaSymbol> symbol;
			if (symbols->symbolByRVA(0, &symbol) != S_OK || !symbol)
			{
				REX::WARN("No first symbol available");
				retflag = 3;
				return;
			}
			while (symbol)
			{
				DWORD rva{};
				(void)symbol->get_relativeVirtualAddress(&rva);
				REX::INFO("{}", describe_symbol(symbol, session.pSession, rva).text);
				symbol.Release();
				ULONG fetched{};
				const auto status = symbols->Next(1, &symbol, &fetched);
				if (FAILED(status))
				{
					REX::WARN("Symbol enumeration failed: {}", print_hr_failure(status));
					retflag = 3;
					return;
				}
				if (fetched != 1)
					break;
			}
		}
	}
}
