#include "Menu/SymbolDiagnostics.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <dia2.h>
#include <atlcomcli.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <expected>
#include <filesystem>
#include <format>
#include <functional>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

namespace CrashUI
{
	namespace
	{
		struct ProbeError : std::runtime_error
		{
			HRESULT code;
			ProbeError(std::string_view a_operation, HRESULT a_code) :
				std::runtime_error(std::format("{} (0x{:08X})", a_operation, static_cast<std::uint32_t>(a_code))),
				code(a_code)
			{}
		};
		struct Cancelled {};

		[[noreturn]] void Fail(std::string_view a_operation, DWORD a_error = GetLastError())
		{
			throw ProbeError(a_operation, HRESULT_FROM_WIN32(a_error));
		}

		std::string Utf8(std::wstring_view a_value)
		{
			if (a_value.empty())
				return {};
			const auto size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, a_value.data(),
				static_cast<int>(a_value.size()), nullptr, 0, nullptr, nullptr);
			if (size == 0)
				Fail("Unicode conversion failed");
			std::string result(static_cast<std::size_t>(size), '\0');
			if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, a_value.data(),
					static_cast<int>(a_value.size()), result.data(), size, nullptr, nullptr) != size)
				Fail("Unicode conversion failed");
			return result;
		}

		std::wstring Wide(std::string_view a_value)
		{
			if (a_value.empty())
				return {};
			if (a_value.size() > 32768)
				Fail("Path exceeds the local probe limit", ERROR_FILENAME_EXCED_RANGE);
			const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, a_value.data(),
				static_cast<int>(a_value.size()), nullptr, 0);
			if (size == 0)
				Fail("Invalid UTF-8 path");
			std::wstring result(static_cast<std::size_t>(size), L'\0');
			if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, a_value.data(),
					static_cast<int>(a_value.size()), result.data(), size) != size)
				Fail("UTF-8 path conversion failed");
			return result;
		}

		void RequireLocal(const std::filesystem::path& a_path)
		{
			auto text = a_path.native();
			if (text.starts_with(L"\\\\?\\UNC\\") || text.starts_with(L"\\??\\UNC\\"))
				Fail("Remote paths are outside this local probe", ERROR_NOT_SUPPORTED);
			if (text.starts_with(L"\\\\?\\"))
				text.erase(0, 4);
			if (text.starts_with(L"\\\\") || text.find(L'\0') != std::wstring::npos)
				Fail("Remote or invalid paths are outside this local probe", ERROR_NOT_SUPPORTED);
			const auto root = std::filesystem::path(text).root_path();
			const auto drive = GetDriveTypeW(root.c_str());
			if (root.empty() || drive == DRIVE_REMOTE || drive == DRIVE_UNKNOWN || drive == DRIVE_NO_ROOT_DIR)
				Fail("Path is not a verified local drive", ERROR_NOT_SUPPORTED);
		}

		struct FileIdentity
		{
			DWORD volume{}, high{}, low{};
			std::uint64_t size{}, writeTime{};
			bool operator==(const FileIdentity&) const noexcept = default;
		};

		class LocalFile
		{
		public:
			HANDLE handle{ INVALID_HANDLE_VALUE };
			std::filesystem::path requested;
			std::filesystem::path path;
			FileIdentity identity;

			explicit LocalFile(const std::filesystem::path& a_path, bool a_directory = false) :
				requested(std::filesystem::absolute(a_path))
			{
				RequireLocal(requested);
				handle = CreateFileW(requested.c_str(), a_directory ? FILE_READ_ATTRIBUTES : GENERIC_READ,
					a_directory ? FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE : FILE_SHARE_READ,
					nullptr, OPEN_EXISTING, a_directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL, nullptr);
				if (handle == INVALID_HANDLE_VALUE)
					Fail("Local file could not be opened");
				try
				{
					std::array<wchar_t, 32768> canonical{};
					const auto length = GetFinalPathNameByHandleW(handle, canonical.data(),
						static_cast<DWORD>(canonical.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
					if (length == 0 || length >= canonical.size())
						Fail("Could not resolve a bounded local path");
					path = std::filesystem::path(std::wstring(canonical.data(), length));
					RequireLocal(path);
					identity = Identity();
				}
				catch (...)
				{
					CloseHandle(handle);
					handle = INVALID_HANDLE_VALUE;
					throw;
				}
			}
			~LocalFile() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
			LocalFile(const LocalFile&) = delete;
			LocalFile& operator=(const LocalFile&) = delete;

			FileIdentity Identity() const
			{
				BY_HANDLE_FILE_INFORMATION info{};
				if (!GetFileInformationByHandle(handle, &info))
					Fail("Local file identity unavailable");
				return { info.dwVolumeSerialNumber, info.nFileIndexHigh, info.nFileIndexLow,
					(static_cast<std::uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow,
					(static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime };
			}

			void CheckUnchanged(bool a_directory = false) const
			{
				LocalFile named(requested, a_directory);
				if (!(Identity() == identity) || !(named.identity == identity))
					Fail("A probe input changed; run a fresh probe", ERROR_FILE_INVALID);
			}

			void Read(std::uint64_t a_offset, void* a_output, std::size_t a_size) const
			{
				if (a_offset > identity.size || a_size > identity.size - a_offset ||
					a_offset > static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)()) ||
					a_size > (std::numeric_limits<DWORD>::max)())
					Fail("Executable metadata is outside the file", ERROR_BAD_EXE_FORMAT);
				LARGE_INTEGER offset{};
				offset.QuadPart = static_cast<LONGLONG>(a_offset);
				DWORD read{};
				if (!SetFilePointerEx(handle, offset, nullptr, FILE_BEGIN) ||
					!ReadFile(handle, a_output, static_cast<DWORD>(a_size), &read, nullptr) || read != a_size)
					Fail("Executable metadata read failed");
			}

			template <class T> T Read(std::uint64_t a_offset) const
			{
				T value{};
				Read(a_offset, &value, sizeof(value));
				return value;
			}
		};

		struct PeInfo
		{
			DWORD entryRva{}, imageSize{}, timestamp{};
			bool hasCodeView{};
			GUID guid{};
			DWORD age{};
			std::wstring pdbName;
			std::string note;
		};

		PeInfo ReadPe(const LocalFile& a_file, bool a_requireDll)
		{
			const auto dos = a_file.Read<IMAGE_DOS_HEADER>(0);
			if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0)
				Fail("Input lacks a PE executable header", ERROR_BAD_EXE_FORMAT);
			const auto nt = a_file.Read<IMAGE_NT_HEADERS64>(static_cast<std::uint64_t>(dos.e_lfanew));
			if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
				nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
				nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) ||
				(a_requireDll && (nt.FileHeader.Characteristics & IMAGE_FILE_DLL) == 0) ||
				nt.FileHeader.NumberOfSections == 0 || nt.FileHeader.NumberOfSections > 96)
				Fail("Input is not the expected x64 PE image", ERROR_BAD_EXE_FORMAT);
			PeInfo result{ nt.OptionalHeader.AddressOfEntryPoint, nt.OptionalHeader.SizeOfImage, nt.FileHeader.TimeDateStamp };
			std::vector<IMAGE_SECTION_HEADER> sections(nt.FileHeader.NumberOfSections);
			a_file.Read(static_cast<std::uint64_t>(dos.e_lfanew) + 24 + nt.FileHeader.SizeOfOptionalHeader,
				sections.data(), sections.size() * sizeof(IMAGE_SECTION_HEADER));
			const auto fileOffset = [&](DWORD a_rva, DWORD a_size) -> std::optional<std::uint64_t> {
				for (const auto& section : sections)
					if (a_rva >= section.VirtualAddress && a_rva - section.VirtualAddress <= section.SizeOfRawData &&
						a_size <= section.SizeOfRawData - (a_rva - section.VirtualAddress))
						return static_cast<std::uint64_t>(section.PointerToRawData) + a_rva - section.VirtualAddress;
				return std::nullopt;
			};
			if (result.entryRva >= result.imageSize)
				result.entryRva = 0;
			if (a_requireDll)
				return result;
			if (std::ranges::none_of(sections, [&](const auto& section) {
					return (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 &&
						result.entryRva >= section.VirtualAddress &&
						result.entryRva - section.VirtualAddress < section.SizeOfRawData;
				}))
				result.entryRva = 0;
			if (nt.OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG)
				return result;
			const auto directory = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
			if (directory.Size == 0)
				return result;
			if (directory.Size > 128 * sizeof(IMAGE_DEBUG_DIRECTORY) || directory.Size % sizeof(IMAGE_DEBUG_DIRECTORY) != 0)
			{
				result.note = "Debug-directory size is unsupported by the bounded probe.";
				return result;
			}
			const auto offset = fileOffset(directory.VirtualAddress, directory.Size);
			if (!offset)
			{
				result.note = "Debug directory is not backed by a readable file section.";
				return result;
			}
			for (DWORD i = 0; i < directory.Size / sizeof(IMAGE_DEBUG_DIRECTORY); ++i)
			{
				const auto entry = a_file.Read<IMAGE_DEBUG_DIRECTORY>(*offset + i * sizeof(IMAGE_DEBUG_DIRECTORY));
				if (entry.Type != IMAGE_DEBUG_TYPE_CODEVIEW || entry.SizeOfData < 25 || entry.SizeOfData > 4096)
					continue;
				std::vector<char> data(entry.SizeOfData);
				a_file.Read(entry.PointerToRawData, data.data(), data.size());
				if (std::memcmp(data.data(), "RSDS", 4) != 0)
					continue;
				if (result.hasCodeView)
				{
					result.hasCodeView = false;
					result.note = "Multiple CodeView identities are ambiguous; identity matching was skipped.";
					return result;
				}
				std::memcpy(&result.guid, data.data() + 4, sizeof(GUID));
				std::memcpy(&result.age, data.data() + 20, sizeof(DWORD));
				const auto nameEnd = std::find(data.begin() + 24, data.end(), '\0');
				if (nameEnd == data.end())
					continue;
				const std::string_view embedded(data.data() + 24, static_cast<std::size_t>(nameEnd - (data.begin() + 24)));
				const auto separator = embedded.find_last_of("\\/");
				try
				{
					result.pdbName = Wide(embedded.substr(separator == std::string_view::npos ? 0 : separator + 1));
					if (result.pdbName.empty() || result.pdbName.size() > 255 ||
						result.pdbName.find_first_of(L"\\/:*?") != std::wstring::npos ||
						_wcsicmp(std::filesystem::path(result.pdbName).extension().c_str(), L".pdb") != 0)
					{
						result.pdbName.clear();
						result.note = "CodeView basename is not a supported local PDB filename; using the executable basename.";
					}
				}
				catch (const ProbeError&)
				{
					result.note = "CodeView filename encoding is unsupported; executable-basename candidates will be used.";
				}
				result.hasCodeView = true;
			}
			return result;
		}

		std::wstring GuidKey(const GUID& a_guid, DWORD a_age)
		{
			std::array<wchar_t, 40> text{};
			if (StringFromGUID2(a_guid, text.data(), static_cast<int>(text.size())) == 0)
				Fail("GUID formatting failed", ERROR_INVALID_DATA);
			std::wstring result;
			for (const auto character : text)
				if (character != L'{' && character != L'}' && character != L'-' && character != L'\0')
					result += character;
			result += std::format(L"{:X}", a_age);
			return result;
		}

		std::optional<std::filesystem::path> RegisteredProvider()
		{
			std::array<wchar_t, 40> id{};
			if (StringFromGUID2(CLSID_DiaSource, id.data(), static_cast<int>(id.size())) == 0)
				Fail("DIA CLSID formatting failed", ERROR_INVALID_DATA);
			const auto key = std::wstring(L"CLSID\\") + id.data() + L"\\InprocServer32";
			DWORD size{};
			const auto flags = RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND;
			auto status = RegGetValueW(HKEY_CLASSES_ROOT, key.c_str(), nullptr, flags, nullptr, nullptr, &size);
			if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND)
				return std::nullopt;
			if (status != ERROR_SUCCESS || size < sizeof(wchar_t) || size > 65536)
				Fail("Registered DIA provider path unavailable", status == ERROR_SUCCESS ? ERROR_INVALID_DATA : status);
			std::vector<wchar_t> text(size / sizeof(wchar_t) + 1, L'\0');
			DWORD type{};
			status = RegGetValueW(HKEY_CLASSES_ROOT, key.c_str(), nullptr, flags, &type, text.data(), &size);
			if (status != ERROR_SUCCESS)
				Fail("Registered DIA provider path unavailable", status);
			std::wstring path(text.data());
			if (type == REG_EXPAND_SZ)
			{
				std::array<wchar_t, 32768> expanded{};
				const auto length = ExpandEnvironmentStringsW(path.c_str(), expanded.data(), static_cast<DWORD>(expanded.size()));
				if (length == 0 || length > expanded.size())
					Fail("Registered provider path expansion failed");
				path = expanded.data();
			}
			if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"')
				path = path.substr(1, path.size() - 2);
			return std::filesystem::path(path);
		}

		struct Apartment
		{
			HRESULT result{ CoInitializeEx(nullptr, COINIT_MULTITHREADED) };
			~Apartment() { if (SUCCEEDED(result)) CoUninitialize(); }
		};

		struct Provider
		{
			std::unique_ptr<LocalFile> file;
			HMODULE module{};
			CComPtr<IClassFactory> factory;
			~Provider()
			{
				factory.Release();
				if (module)
					FreeLibrary(module);
			}
		};

		std::unique_ptr<Provider> LoadProvider(const std::filesystem::path& a_path)
		{
			auto provider = std::make_unique<Provider>();
			provider->file = std::make_unique<LocalFile>(a_path);
			(void)ReadPe(*provider->file, true);
			provider->module = LoadLibraryExW(provider->file->path.c_str(), nullptr,
				LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
			if (!provider->module)
				Fail("DIA provider could not be loaded under the local DLL policy");
			std::array<wchar_t, 32768> loadedPath{};
			const auto loadedLength = GetModuleFileNameW(provider->module, loadedPath.data(), static_cast<DWORD>(loadedPath.size()));
			if (loadedLength == 0 || loadedLength >= loadedPath.size())
				Fail("Loaded provider path unavailable");
			LocalFile loaded(std::filesystem::path(std::wstring(loadedPath.data(), loadedLength)));
			if (!(loaded.identity == provider->file->identity))
				Fail("Loaded provider differs from the validated local file", ERROR_FILE_INVALID);
			using FactoryFunction = HRESULT(WINAPI*)(REFCLSID, REFIID, void**);
			const auto function = reinterpret_cast<FactoryFunction>(GetProcAddress(provider->module, "DllGetClassObject"));
			if (!function)
				Fail("DIA provider has no class factory");
			const auto result = function(CLSID_DiaSource, IID_IClassFactory, reinterpret_cast<void**>(&provider->factory));
			if (FAILED(result) || !provider->factory)
				throw ProbeError("DIA class factory unavailable", FAILED(result) ? result : E_UNEXPECTED);
			return provider;
		}

		void Probe(const SymbolProbeRequest& a_request, SymbolProbeSnapshot& result, const std::function<bool()>& a_current)
		{
			result.request = a_request;
			SYSTEMTIME now{};
			GetSystemTime(&now);
			result.observedAt = std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02} UTC",
				now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
			const auto add = [&](std::string a_label, std::string a_value, DiagnosticTone a_tone = DiagnosticTone::kNormal) {
				if (a_value.size() > 4096)
				{
					std::size_t end = 4096;
					while (end != 0 && (static_cast<unsigned char>(a_value[end]) & 0xC0u) == 0x80u)
						--end;
					a_value.resize(end);
					a_value += " [display truncated]";
					a_tone = DiagnosticTone::kWarning;
				}
				result.fields.push_back({ std::move(a_label), std::move(a_value), a_tone });
			};
			const auto check = [&] { if (!a_current()) throw Cancelled{}; };
			add("Scope", "Current executable on disk, not a historical crash. Loaded-image equivalence is not assumed.");
			add("Search policy", "Explicit verified-local PDB files only. No automatic DIA search, symbol servers, source-file access or downloads.");
			add("Current logger", a_request.loggerEnabled ? "Enabled for this session" : "Disabled for this session");
			add("Game runtime at startup", a_request.gameVersion);
			add("Logger version at startup", a_request.loggerVersion);
			check();
			std::array<wchar_t, 32768> executablePath{};
			const auto length = GetModuleFileNameW(nullptr, executablePath.data(), static_cast<DWORD>(executablePath.size()));
			if (length == 0 || length >= executablePath.size())
				Fail("Current executable path unavailable");
			LocalFile executable(std::filesystem::path(std::wstring(executablePath.data(), length)));
			const auto pe = ReadPe(executable, false);
			add("Executable", Utf8(executable.path.native()));
			add("PE identity", std::format("x64; timestamp=0x{:08X}; image size={}", pe.timestamp, pe.imageSize));
			add("Executable CodeView", pe.hasCodeView ? Utf8(GuidKey(pe.guid, pe.age)) : "Unavailable; a runtime/PDB identity match cannot be established.",
				pe.hasCodeView ? DiagnosticTone::kNormal : DiagnosticTone::kWarning);
			if (!pe.note.empty())
				add("Executable metadata", pe.note, DiagnosticTone::kWarning);
			Apartment apartment;
			if (FAILED(apartment.result))
				throw ProbeError("Diagnostic COM apartment unavailable", apartment.result);
			std::unique_ptr<Provider> provider;
			const auto pluginDirectory = std::filesystem::absolute(L"Data\\F4SE\\Plugins");
			try
			{
				provider = LoadProvider(pluginDirectory / L"msdia140.dll");
				add("DIA provider source", "Bundled provider", DiagnosticTone::kGood);
			}
			catch (const ProbeError& failure)
			{
				add("Bundled DIA", failure.what(), DiagnosticTone::kWarning);
				check();
				const auto registered = RegisteredProvider();
				if (!registered)
					throw ProbeError("No bundled or registered DIA provider available", REGDB_E_CLASSNOTREG);
				provider = LoadProvider(*registered);
				add("DIA provider source", "Verified-local registered provider, loaded directly; registry unchanged", DiagnosticTone::kGood);
			}
			add("DIA provider", Utf8(provider->file->path.native()));
			std::vector<std::filesystem::path> roots;
			for (const auto& path : {pluginDirectory, executable.path.parent_path()})
			{
				try { LocalFile folder(path, true); roots.push_back(folder.path); }
				catch (const ProbeError& failure) { add("Local search directory skipped", failure.what(), DiagnosticTone::kWarning); }
			}
			if (!a_request.activeCacheDirectory.empty())
			{
				add("Active cache setting", a_request.activeCacheDirectory);
				try
				{
					LocalFile cache(std::filesystem::path(Wide(a_request.activeCacheDirectory)), true);
					roots.push_back(cache.path);
					add("Local cache", Utf8(cache.path.native()), DiagnosticTone::kGood);
				}
				catch (const ProbeError& failure) { add("Local cache skipped", failure.what(), DiagnosticTone::kWarning); }
			}
			else
				add("Active cache setting", "Empty");
			std::vector<std::wstring> names{ executable.path.stem().wstring() + L".pdb" };
			if (!pe.pdbName.empty() && std::ranges::find(names, pe.pdbName) == names.end())
				names.push_back(pe.pdbName);
			std::vector<std::filesystem::path> candidates;
			for (const auto& root : roots)
				for (const auto& name : names)
				{
					candidates.push_back(root / name);
					if (pe.hasCodeView)
						candidates.push_back(root / name / GuidKey(pe.guid, pe.age) / name);
				}
			std::unique_ptr<LocalFile> chosenFile;
			CComPtr<IDiaDataSource> chosen;
			std::size_t foundCandidates{};
			for (const auto& candidate : candidates)
			{
				check();
				try
				{
					auto file = std::make_unique<LocalFile>(candidate);
					++foundCandidates;
					if (file->identity.size > 256ull * 1024 * 1024)
					{
						add("PDB candidate skipped", "Exceeds the 256 MiB local probe limit.", DiagnosticTone::kWarning);
						continue;
					}
					CComPtr<IDiaDataSource> source;
					auto status = provider->factory->CreateInstance(nullptr, __uuidof(IDiaDataSource), reinterpret_cast<void**>(&source));
					if (FAILED(status) || !source)
						throw ProbeError("DIA data source unavailable", FAILED(status) ? status : E_UNEXPECTED);
					if (pe.hasCodeView)
					{
						auto guid = pe.guid;
						status = source->loadAndValidateDataFromPdb(file->path.c_str(), &guid, 0, pe.age);
					}
					else
						status = source->loadDataFromPdb(file->path.c_str());
					if (FAILED(status))
					{
						add("PDB candidate rejected", Utf8(file->path.native()) + " - " +
							std::format("DIA result 0x{:08X}", static_cast<std::uint32_t>(status)), DiagnosticTone::kWarning);
						continue;
					}
					chosenFile = std::move(file);
					chosen = std::move(source);
					break;
				}
				catch (const ProbeError& failure)
				{
					if (failure.code != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) &&
						failure.code != HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
						add("PDB candidate unavailable", failure.what(), DiagnosticTone::kWarning);
				}
			}
			check();
			if (!chosen)
			{
				add("Local PDB result", foundCandidates == 0 ?
					"No PDB found at the explicit local candidate paths." :
					"No candidate was accepted by DIA.", DiagnosticTone::kWarning);
				result.compactSummary = "Local symbol probe: no accepted local PDB. This is not a diagnosis of a historical crash.";
			}
			else
			{
				add("Opened PDB", Utf8(chosenFile->path.native()), DiagnosticTone::kGood);
				add("Executable/PDB identity", pe.hasCodeView ?
					"GUID and age accepted by DIA validation against the executable CodeView record." :
					"Not verified: the executable has no supported CodeView identity.",
					pe.hasCodeView ? DiagnosticTone::kGood : DiagnosticTone::kWarning);
				CComPtr<IDiaSession> session;
				auto status = chosen->openSession(&session);
				if (FAILED(status) || !session)
					throw ProbeError("DIA session unavailable", FAILED(status) ? status : E_UNEXPECTED);
				if (pe.hasCodeView && pe.entryRva != 0)
				{
					CComPtr<IDiaSymbol> symbol;
					status = session->findSymbolByRVA(pe.entryRva, SymTagFunction, &symbol);
					DWORD symbolRva{};
					ULONGLONG symbolLength{};
					if (status == S_OK && symbol &&
						(symbol->get_relativeVirtualAddress(&symbolRva) != S_OK ||
							symbol->get_length(&symbolLength) != S_OK || symbolLength == 0 ||
							pe.entryRva < symbolRva || pe.entryRva - symbolRva >= symbolLength))
						symbol.Release();
					bool publicSymbol{};
					if (status != S_OK || !symbol)
					{
						symbol.Release();
						status = session->findSymbolByRVA(pe.entryRva, SymTagPublicSymbol, &symbol);
						publicSymbol = true;
					}
					add("Sample RVA", std::format("0x{:X} (on-disk executable entry point; one sample only)", pe.entryRva));
					CComBSTR name;
					if (FAILED(status))
						add("Sample symbol", std::format("Lookup failed (0x{:08X}).", static_cast<std::uint32_t>(status)), DiagnosticTone::kWarning);
					else if (status == S_OK && symbol)
					{
						const auto nameStatus = symbol->get_name(&name);
						const auto addressStatus = symbol->get_relativeVirtualAddress(&symbolRva);
						if (nameStatus == S_OK && name && addressStatus == S_OK && symbolRva <= pe.entryRva)
							add(publicSymbol ? "Public symbol at/before sample" : "Function containing sample",
								Utf8(std::wstring_view(name, name.Length())) +
									std::format(" +0x{:X}", pe.entryRva - symbolRva), DiagnosticTone::kNormal);
						else
							add("Sample symbol", "A symbol was returned but its name/address metadata was unavailable or inconsistent.", DiagnosticTone::kWarning);
					}
					else
						add("Sample symbol", "No symbol at the sampled RVA; this does not invalidate the PDB.", DiagnosticTone::kNormal);
					CComPtr<IDiaEnumLineNumbers> lines;
					CComPtr<IDiaLineNumber> line;
					ULONG fetched{};
					DWORD lineNumber{};
					DWORD lineRva{}, lineLength{};
					auto lineStatus = session->findLinesByRVA(pe.entryRva, 1, &lines);
					if (lineStatus == S_OK && lines)
						lineStatus = lines->Next(1, &line, &fetched);
					if (FAILED(lineStatus))
						add("Sample source line", std::format("Line lookup failed (0x{:08X}).", static_cast<std::uint32_t>(lineStatus)), DiagnosticTone::kWarning);
					else if (fetched == 1 && line)
					{
						if (line->get_lineNumber(&lineNumber) == S_OK &&
							line->get_relativeVirtualAddress(&lineRva) == S_OK &&
							line->get_length(&lineLength) == S_OK && lineLength != 0 &&
							pe.entryRva >= lineRva && pe.entryRva - lineRva < lineLength)
							add("Sample source line", std::format("Line {} is available in the PDB.", lineNumber));
						else
							add("Sample source line", "Returned line metadata does not establish coverage of the sample.", DiagnosticTone::kWarning);
					}
					else
						add("Sample source line", "No source line for this sample. Public-symbol-only PDBs remain useful.");
				}
				else
					add("Sample lookup", "Skipped: no matched identity or suitable entry-point RVA.");
				chosenFile->CheckUnchanged();
				result.compactSummary = pe.hasCodeView ?
					"Local symbol probe: a PDB was accepted against the executable GUID/age. Sample results are not whole-module coverage." :
					"Local symbol probe: a PDB opened, but executable identity matching was unavailable.";
			}
			provider->file->CheckUnchanged();
			executable.CheckUnchanged();
			check();
		}
	}

	SymbolDiagnostics& SymbolDiagnostics::GetSingleton()
	{
		static auto* instance = new SymbolDiagnostics;
		return *instance;
	}

	SymbolDiagnostics::SymbolDiagnostics() : m_snapshot(std::make_shared<const SymbolProbeSnapshot>())
	{
		auto failure = std::make_shared<SymbolProbeSnapshot>();
		failure->phase = ProbePhase::kFinished;
		failure->error = "Symbol diagnostics ran out of memory; no result is available.";
		m_allocationFailure = std::move(failure);
		std::thread([this] { Worker(); }).detach();
	}

	void SymbolDiagnostics::RequestProbe(SymbolProbeRequest a_request)
	{
		if (a_request.activeCacheDirectory.size() > 32768 ||
			a_request.gameVersion.size() > 512 || a_request.loggerVersion.size() > 512)
			throw std::invalid_argument("Symbol probe input exceeds its bounded display/path limits");
		{
			std::scoped_lock lock(m_mutex);
			auto queued = std::make_shared<SymbolProbeSnapshot>();
			queued->generation = m_generation + 1;
			queued->phase = ProbePhase::kQueued;
			queued->workerBusy = m_busy;
			queued->pending = true;
			queued->request = a_request;
			m_pending = Request{ queued->generation, std::move(a_request) };
			m_generation = queued->generation;
			m_cancelledIdle.reset();
			m_snapshot = std::move(queued);
		}
		m_condition.notify_one();
	}

	void SymbolDiagnostics::CancelProbe()
	{
		std::scoped_lock lock(m_mutex);
		auto cancelled = std::make_shared<SymbolProbeSnapshot>();
		auto idle = std::make_shared<SymbolProbeSnapshot>();
		cancelled->generation = m_generation + 1;
		cancelled->phase = ProbePhase::kCancelled;
		cancelled->workerBusy = m_busy;
		cancelled->error = m_busy ? "Request abandoned; the worker may still be finishing a synchronous operation." : "Request cancelled.";
		idle->generation = cancelled->generation;
		idle->phase = ProbePhase::kCancelled;
		idle->error = "Request cancelled; worker is idle.";
		++m_generation;
		m_cancelledIdle = std::move(idle);
		m_pending.reset();
		m_snapshot = std::move(cancelled);
	}

	std::shared_ptr<const SymbolProbeSnapshot> SymbolDiagnostics::Snapshot() const
	{
		std::scoped_lock lock(m_mutex);
		return m_snapshot;
	}

	bool SymbolDiagnostics::IsCurrent(std::uint64_t a_generation) const
	{
		std::scoped_lock lock(m_mutex);
		return a_generation == m_generation;
	}

	void SymbolDiagnostics::Worker()
	{
		for (;;)
		{
			Request request;
			{
				std::unique_lock lock(m_mutex);
				m_condition.wait(lock, [&] { return m_pending.has_value(); });
				request = std::move(*m_pending);
				m_pending.reset();
				m_busy = true;
			}
			std::shared_ptr<SymbolProbeSnapshot> result;
			try
			{
				auto running = std::make_shared<SymbolProbeSnapshot>();
				running->generation = request.generation;
				running->request = request.values;
				running->phase = ProbePhase::kRunning;
				running->workerBusy = true;
				{
					std::scoped_lock lock(m_mutex);
					if (request.generation == m_generation)
						m_snapshot = std::move(running);
				}
				result = std::make_shared<SymbolProbeSnapshot>();
				result->generation = request.generation;
				result->request = request.values;
				try
				{
					Probe(request.values, *result, [&] { return IsCurrent(request.generation); });
					result->phase = ProbePhase::kFinished;
				}
				catch (const Cancelled&)
				{
					result->phase = ProbePhase::kCancelled;
				}
				catch (const ProbeError& failure)
				{
					if (failure.code == HRESULT_FROM_WIN32(ERROR_FILE_INVALID))
						result->fields.clear();
					result->phase = ProbePhase::kFinished;
					result->error = failure.what();
					result->compactSummary = "Local symbol probe could not complete. No historical-crash conclusion is available.";
				}
				catch (const std::exception& failure)
				{
					result->phase = ProbePhase::kFinished;
					result->error = failure.what();
					result->compactSummary = "Local symbol probe could not complete. No historical-crash conclusion is available.";
				}
			}
			catch (const std::bad_alloc&)
			{
				result.reset();
				OutputDebugStringW(L"Addictol Crash Logger: symbol diagnostics exhausted memory.\n");
			}
			{
				std::scoped_lock lock(m_mutex);
				m_busy = false;
				if (m_generation == request.generation)
					m_snapshot = result ? std::move(result) : m_allocationFailure;
				else if (!m_pending && m_cancelledIdle)
					m_snapshot = m_cancelledIdle;
			}
		}
	}
}
