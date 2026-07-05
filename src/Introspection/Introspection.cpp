#include "Introspection/Introspection.h"

#include "Introspection/HeapAnalysis.h"
#include "Modules/ModuleHandler.h"
#include "PDB/PdbHandler.h"
#define MAGIC_ENUM_RANGE_MAX 256
#include <magic_enum.hpp>

namespace Crash::Introspection::F4
{
	using filter_results = std::vector<std::pair<std::string, std::string>>;

	[[nodiscard]] std::string quoted(std::string_view a_str)
	{
		return fmt::format("\"{}\""sv, a_str);
	}

	[[nodiscard]] std::string truncate_string(std::string_view a_str, std::size_t a_max)
	{
		if (a_str.size() <= a_max) {
			return std::string(a_str);
		}
		if (a_max <= 3) {
			return std::string(a_str.substr(0, a_max));
		}
		return fmt::format("{}..."sv, a_str.substr(0, a_max - 3));
	}

	namespace BSResource
	{
		class LooseFileStreamBase
		{
		public:
			using value_type = RE::BSResource::LooseFileStreamBase;

			static void filter(
				filter_results& a_results,
				const void* a_ptr, int tab_depth = 0) noexcept
			{
				const auto stream = static_cast<const value_type*>(a_ptr);

				try {
					const auto dirName = stream->GetDirName();
					if (!dirName.empty())
						a_results.emplace_back(
							fmt::format(
								"{:\t>{}}Directory Name"sv,
								"",
								tab_depth),
							quoted(dirName));
				}
				catch (...) {}

				try {
					const auto fileName = stream->GetFileName();
					if (!fileName.empty())
						a_results.emplace_back(
							fmt::format(
								"{:\t>{}}File Name"sv,
								"",
								tab_depth),
							quoted(fileName));
				}
				catch (...) {}

				try {
					const auto prefix = stream->GetPrefix();
					if (!prefix.empty())
						a_results.emplace_back(
							fmt::format(
								"{:\t>{}}Prefix"sv,
								"",
								tab_depth),
							quoted(prefix));
				}
				catch (...) {}
			}
		};
	}

	namespace BSScript
	{
		namespace NF_util
		{
			class NativeFunctionBase
			{
			public:
				using value_type = RE::BSScript::NF_util::NativeFunctionBase;

				static void filter(
					filter_results& a_results,
					const void* a_ptr, int tab_depth = 0) noexcept
				{
					const auto function = static_cast<const value_type*>(a_ptr);

					try {
						const std::string_view name = function->GetName();
						if (!name.empty())
							a_results.emplace_back(
								fmt::format(
									"{:\t>{}}Function"sv,
									"",
									tab_depth),
								quoted(name));
					}
					catch (...) {}

					try {
						const std::string_view objName = function->GetObjectTypeName();
						if (!objName.empty())
							a_results.emplace_back(
								fmt::format(
									"{:\t>{}}Object"sv,
									"",
									tab_depth),
								quoted(objName));
					}
					catch (...) {}

					try {
						const std::string_view stateName = function->GetStateName();
						if (!stateName.empty())
							a_results.emplace_back(
								fmt::format(
									"{:\t>{}}State"sv,
									"",
									tab_depth),
								quoted(stateName));

					}
					catch (...) {}
				}
			};
		}

		class ObjectTypeInfo
		{
		public:
			using value_type = RE::BSScript::ObjectTypeInfo;

			static void filter(
				filter_results& a_results,
				const void* a_ptr, int tab_depth = 0) noexcept
			{
				const auto info = static_cast<const value_type*>(a_ptr);

				try {
					const std::string_view name = info->name;
					if (!name.empty())
						a_results.emplace_back(
							fmt::format(
								"{:\t>{}}Name"sv,
								"",
								tab_depth),
							quoted(name));
				}
				catch (...) {}

				try {
					const std::string_view docString = info->docString;
					if (!docString.empty())
						a_results.emplace_back(
							fmt::format(
								"{:\t>{}}DocString"sv,
								"",
								tab_depth),
							quoted(docString));
				}
				catch (...) {}
			}
		};
	}

	class NiObjectNET
	{
	public:
		using value_type = RE::NiObjectNET;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto object = static_cast<const value_type*>(a_ptr);

			try {
				const auto name = object->GetName();
				if (!name.empty())
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Name"sv,
							"",
							tab_depth),
						quoted(name));
			}
			catch (...) {}
		}
	};

	class NiStream
	{
	public:
		using value_type = RE::NiStream;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto object = static_cast<const value_type*>(a_ptr);
			if (!object)
				return;
			try {
				const auto fileName = object->GetFileName();
				if (!fileName.empty())
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}File Name"sv,
							""sv,
							tab_depth),
						quoted(fileName));
			}
			catch (...) {}
			try {
				const auto& header = object->bsStreamHeader;
				a_results.emplace_back(
					fmt::format(
						"{:\t>{}}Header"sv,
						"",
						tab_depth),
					fmt::format(
						"author: {} version: {} processScript: {} exportScript: {}",
						header.author,
						header.version,
						header.processScript,
						header.exportScript));
			}
			catch (...) {}

			try {
				const auto lastLoadedRTTI = object->lastLoadedRTTI;
				if (lastLoadedRTTI && lastLoadedRTTI[0])
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}lastLoadedRTTI"sv,
							"",
							tab_depth),
						quoted(lastLoadedRTTI));
			}
			catch (...) {}

			try {
				const auto fileName = object->fileName;
				if (fileName && fileName[0])
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}fileName"sv,
							"",
							tab_depth),
						quoted(fileName));
			}
			catch (...) {}

			try {
				const auto filePath = object->filePath;
				if (filePath && filePath[0])
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}filePath"sv,
							"",
							tab_depth),
						quoted(filePath));
			}
			catch (...) {}
		}
	};

	class NiTexture
	{
	public:
		using value_type = RE::NiTexture;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto object = static_cast<const value_type*>(a_ptr);
			if (!object)
				return;
			try {
				const auto name = object ? object->name.c_str() : ""sv;
				if (!name.empty())
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Name"sv,
							"",
							tab_depth),
						quoted(name));
			}
			catch (...) {}

			try {
				const auto name = object->GetRTTI() ? object->GetRTTI()->GetName() : ""sv;
				if (!name.empty())
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}RTTIName"sv,
							"",
							tab_depth),
						quoted(name));
			}
			catch (...) {}
		}
	};

	template <class T = RE::TESForm>
	class TESForm
	{
	public:
		using value_type = T;

		static void filter(filter_results& a_results, const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto form = static_cast<const value_type*>(a_ptr);

			// Filename
			try
			{
				const auto file = form->GetDescriptionOwnerFile();
				const auto filename = file ? file->GetFilename() : ""sv;
				if (!filename.empty())
					a_results.emplace_back(fmt::format("{:\t>{}}File"sv, ""sv, tab_depth), quoted(filename));
			}
			catch (...) {}

			// Modified by
			try
			{
				auto sourcefiles = form->sourceFiles.array;
				if (sourcefiles && sourcefiles->size() > unsigned(1))
				{
					std::string filesString = "";
					for (auto index = unsigned(0); index < sourcefiles->size(); index++)
					{
						auto sourcefile = sourcefiles->data()[index];
						filesString = filesString.empty() ? fmt::format("{}"sv, sourcefile->GetFilename().data()) : fmt::format("{} -> {}"sv, filesString, sourcefile->GetFilename().data());
					}

					a_results.emplace_back(fmt::format("{:\t>{}}Modified by"sv, "", tab_depth), filesString);
				}
			}
			catch (...) {}

			// Form Flags
			try
			{
				const auto formFlags = form->GetFormFlags();
                a_results.emplace_back(fmt::format("{:\t>{}}Flags"sv, ""sv, tab_depth), fmt::format("0x{:08X}"sv, formFlags));
            }
			catch (...) {}

			// Object Type Name
			try
			{
				const auto name = form->GetObjectTypeName();
				if (name && name[0])
					a_results.emplace_back(fmt::format("{:\t>{}}Name"sv, "", tab_depth), quoted(name));
			}
			catch (...) {}

			// Form Editor ID
			try
			{
				const auto editorID = form->GetFormEditorID();
				if (editorID && editorID[0])
					a_results.emplace_back(fmt::format("{:\t>{}}EditorID"sv, ""sv, tab_depth), quoted(editorID));
			}
			catch (...) {}

			// Form ID
			try
			{
				const auto formID = form->GetFormID();
				a_results.emplace_back(fmt::format("{:\t>{}}FormID"sv, ""sv, tab_depth), fmt::format("0x{:08X}"sv, formID));
			}
			catch (...) {}

			// Form Type
			try
			{
				const auto formType = form->GetFormType();
				const auto formTypeName = magic_enum::enum_name(formType);
				if (!formTypeName.empty())
					a_results.emplace_back(fmt::format("{:\t>{}}FormType"sv, "", tab_depth), fmt::format("{} ({:02})"sv, formTypeName, std::to_underlying(formType)));
			}
			catch (...) {}
		}
	};

	class TESFullName
	{
	public:
		using value_type = RE::TESFullName;

		static void filter(filter_results& a_results, const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto object = static_cast<const value_type*>(a_ptr);
			if (!object)
				return;

			// Full Name
			try
			{
				const auto name = object->GetFullName();
				if (name && name[0])
					a_results.emplace_back(fmt::format("{:\t>{}}GetFullName"sv, "", tab_depth), quoted(name));
			}
			catch (...) {}
		}
	};

	class TESObjectREFR
	{
	public:
		using value_type = RE::TESObjectREFR;

		static void filter(filter_results& a_results, const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto ref = static_cast<const value_type*>(a_ptr);

			// Object Reference
			try
			{
				const auto objRef = ref->data.objectReference;
				if (objRef)
				{
					filter_results xResults;
					TESForm<RE::TESForm>::filter(xResults, objRef, tab_depth);

					if (!xResults.empty())
					{
						a_results.emplace_back(fmt::format("{:\t>{}}Object Reference"sv, "", tab_depth), "");
						for (auto& [key, value] : xResults)
						{
							a_results.emplace_back(fmt::format("{:\t>{}}{}"sv, "", tab_depth, key), std::move(value));
						}
					}
				}
				else
					a_results.emplace_back(fmt::format("{:\t>{}}Object Reference"sv, ""sv, tab_depth), "None"sv);
			}
			catch (...) {}

			// Leveled Base Form
			try
			{
				RE::ExtraLeveledCreature* leveledCreature = ref->extraList->GetByType<RE::ExtraLeveledCreature>();
				if (leveledCreature && leveledCreature->originalBase)
				{
					filter_results xResults;
					TESForm<RE::TESForm>::filter(xResults, leveledCreature->originalBase, tab_depth + 1);

					if (!xResults.empty())
					{
						a_results.emplace_back(fmt::format("{:\t>{}}Leveled Base Form"sv, "", tab_depth), "");
						for (auto& [key, value] : xResults)
						{
							a_results.emplace_back(fmt::format("{:\t>{}}{}"sv, "", tab_depth, key), std::move(value));
						}
					}
				}
			}
			catch (...) {}

			// Parent Cell
			try {
				const auto parentCell = ref->GetParentCell();
				if (parentCell)
				{
					a_results.emplace_back(fmt::format("{:\t>{}}Parent Cell"sv, "", tab_depth), "");
					TESForm<RE::TESObjectCELL>::filter(a_results, parentCell, tab_depth + 1);
				}
				else
					a_results.emplace_back(fmt::format("{:\t>{}}Parent Cell"sv, "", tab_depth), "None");
			}
			catch (...) {}
		}
	};

	class BSShaderProperty
	{
	public:
		using value_type = RE::BSShaderProperty;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto form = static_cast<const value_type*>(a_ptr);

			try {
				const auto formFlags = form->flags;
				a_results.emplace_back(
					fmt::format(
						fmt::runtime("{:\t>{}}Flags"),
						""sv,
						tab_depth),
					fmt::format(
						fmt::runtime("0x{:08X}"),
						formFlags.underlying()));
			}
			catch (...) {}
			try {
				const auto name = form->name.c_str();
				if (name && name[0])
					a_results.emplace_back(
						fmt::format(
							fmt::runtime("{:\t>{}}Name"),
							""sv,
							tab_depth),
						quoted(name));
			}
			catch (...) {}
			try {
				const auto rttiname = form->GetRTTI() ? form->GetRTTI()->GetName() : ""sv;
				if (!rttiname.empty())
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}RTTIName"sv,
							""sv,
							tab_depth),
						quoted(rttiname));
			}
			catch (...) {}
		}
	};
	class NiAVObject
	{
	public:
		using value_type = RE::NiAVObject;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto object = static_cast<const value_type*>(a_ptr);
			if (!object)
				return;
			try {
				const auto name = object ? object->name.c_str() : ""sv;
				if (!name.empty())
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Name"sv,
							""sv,
							tab_depth),
						quoted(name));
			}
			catch (...) {}

			try {
				const auto name = object->GetRTTI() ? object->GetRTTI()->GetName() : ""sv;
				if (!name.empty())
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}RTTIName"sv,
							""sv,
							tab_depth),
						quoted(name));
			}
			catch (...) {}

			try {
				const auto flags = object->GetFlags();
				a_results.emplace_back(
					fmt::format(
						"{:\t>{}}Flags"sv,
						""sv,
						tab_depth),
					fmt::format(
						"{0:x}"sv,
						flags));
			}
			catch (...) {}

			try {
				const auto objectRefr = RE::TESObjectREFR::FindReferenceFor3D((RE::NiAVObject*)a_ptr);
				if (objectRefr) {
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Checking TESObjectREFR"sv,
							"",
							tab_depth),
						""sv);
					TESObjectREFR::filter(a_results, objectRefr, tab_depth + 1);
				}
			}
			catch (...) {}

			try {
				const auto parent = object->parent;
				if (parent) {
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Checking Parent"sv,
							""sv,
							tab_depth),
						""sv);
					filter(a_results, parent, tab_depth + 1);
				}
			}
			catch (...) {}
		}
	};

	class TESQuest
	{
	public:
		using value_type = RE::TESQuest;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto object = static_cast<const value_type*>(a_ptr);
			if (!object)
				return;
			try {
				a_results.emplace_back(
					fmt::format(
						fmt::runtime("{:\t>{}}Already Run Quest"),
						"",
						tab_depth),
					fmt::format(
						"{}",
						object->alreadyRun));
				a_results.emplace_back(
					fmt::format(
						fmt::runtime("{:\t>{}}Current Stage"),
						"",
						tab_depth),
					fmt::format(
						"{}",
						object->currentStage));
			}
			catch (...) {}
		};
	};

	class ExtraTextDisplayData
	{
	public:
		using value_type = RE::ExtraTextDisplayData;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto object = static_cast<const value_type*>(a_ptr);

			try {
				const auto name = object->displayName.c_str();
				if (name && name[0])
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Display Name"sv,
							"",
							tab_depth),
						quoted(name));
			}
			catch (...) {}
			try {
				const auto& displayNameText = object->displayNameText;
				if (displayNameText)
					TESForm<RE::BGSMessage>::filter(a_results, displayNameText, tab_depth + 1);
			}
			catch (...) {}
			try {
				const auto quest = object->ownerQuest;
				if (quest) {
					a_results.emplace_back(
						fmt::format(
							"{:\t>{}}Owner Quest"sv,
							""sv,
							tab_depth),
						""sv);
					TESQuest::filter(a_results, quest, tab_depth + 1);
				}
			}
			catch (...) {}
		};
	};

	// Code from Nightfallstorm
	class CodeTasklet
	{
	public:
		using value_type = RE::BSScript::Internal::CodeTasklet;

		static void filter(
			filter_results& a_results,
			const void* a_ptr, int tab_depth = 0) noexcept
		{
			const auto object = static_cast<const value_type*>(a_ptr);
			const auto& handlePolicy = RE::BSScript::Internal::VirtualMachine::GetSingleton()->handlePolicy;
			const auto datahandler = RE::TESDataHandler::GetSingleton();
			try {
				auto currentStackFrame = object->stack->top;  // get stack from BSScript::Internal::CodeTasklet (or get stack directly if it's a stack object
				std::string stackTrace = "\n";
				std::map<std::string, bool> objectReferences;
				while (currentStackFrame) {
					auto function = currentStackFrame->owningFunction;
					auto functionObjecTypeName = function.get()->GetObjectTypeName();
					auto functionName = function.get()->GetName();
					auto objectInstanceString = RE::BSFixedString("None");
					auto objectRef = currentStackFrame->self;
					if (objectRef.GetType().IsObject()) {
						auto objectHandle = std::size_t(objectRef.GetType().GetObjectTypeInfo()->data);
						handlePolicy->ConvertHandleToString(objectHandle, objectInstanceString);
						const auto handleString = std::string{ objectInstanceString };
						const auto paranStart = handleString.find("(");
						const auto paranEnd = handleString.rfind(")");
						const auto formIDString = (paranStart != std::string::npos && paranEnd != std::string::npos && paranStart <= paranEnd) ? handleString.substr(paranStart + 1, paranEnd - 1) : "";
						if (!formIDString.empty())
							objectReferences.emplace(formIDString, true);
					}
					auto sourceFileName = function->GetSourceFilename();
					auto traceFormatString = "{:\t>{}}[{}].{}.{}() - \"{}\" Line {}\n";  // Same format in Papyrus logs
					std::string lineTrace = "";
					if (function.get()->GetIsNative()) {
						lineTrace = fmt::format(fmt::runtime(traceFormatString), "", tab_depth, objectInstanceString.c_str(), functionObjecTypeName.c_str(), functionName.c_str(), sourceFileName.c_str(), "?"sv);
					}
					else {
						std::uint32_t lineNumber;
						function.get()->TranslateIPToLineNumber(currentStackFrame->ip, lineNumber);
						lineTrace = fmt::format(fmt::runtime(traceFormatString), "", tab_depth, objectInstanceString.c_str(), functionObjecTypeName.c_str(), functionName.c_str(), sourceFileName.c_str(), std::to_string(lineNumber));
					}
					stackTrace = stackTrace + lineTrace;
					currentStackFrame = currentStackFrame->previousFrame;
				}
				a_results.emplace_back(
					fmt::format(
						"{:\t>{}}Stack Trace"sv,
						"",
						tab_depth),
					stackTrace);
				for (auto& objectReference : objectReferences) {
					const auto objectString = objectReference.first;
					const auto modIndex = (std::uint8_t)std::stoi(objectString.substr(0, 2), nullptr, 16);
					const auto form = (std::uint8_t)std::stoi(objectString.substr(3, objectString.size()), nullptr, 16);
					const auto target = datahandler->LookupForm(form, datahandler->LookupLoadedModByIndex(modIndex)->GetFilename());
					if (target)
						TESForm<RE::TESForm>::filter(a_results, target, tab_depth + 1);
				}
			}
			catch (...) {}
		};
	};
}

namespace Crash::Introspection
{
	[[nodiscard]] const Modules::Module* get_module_for_pointer(
		const void* a_ptr,
		std::span<const module_pointer> a_modules) noexcept
	{
		const auto it = std::lower_bound(
			a_modules.rbegin(),
			a_modules.rend(),
			reinterpret_cast<std::uintptr_t>(a_ptr),
			[](auto&& a_lhs, auto&& a_rhs) noexcept {
				return a_lhs->address() >= a_rhs;
			});
		return it != a_modules.rend() && (*it)->in_range(a_ptr) ? it->get() : nullptr;
	}

	namespace detail
	{
		struct SeenObjectInfo
		{
			std::string result;
			std::size_t first_seen_pos;
			std::string first_seen_label;  // Store the label string to avoid recalculation issues across blocks. Must be initialized at the same time as first_seen_pos to ensure consistency.
			bool is_game_object;           // True for polymorphic game objects, false for void* with module info
		};
		static std::unordered_map<const void*, SeenObjectInfo> seen_objects;
		static std::mutex seen_objects_mutex;  // Protects seen_objects from race conditions
		static std::function<std::string(size_t)> label_generator;
		static thread_local std::size_t current_analysis_pos = 0;
		static std::size_t total_backfill_count = 0;
		static bool backfill_logged_this_crash = false;

		// Generate a label for the current position
		// Uses label_generator if available, otherwise falls back to address string
		// Uses thread_local current_analysis_pos so each thread gets its own position; overall thread-safety depends on label_generator
		[[nodiscard]] inline std::string generate_current_label(const void* a_ptr)
		{
			return label_generator ? label_generator(current_analysis_pos) : fmt::format("0x{:X}", reinterpret_cast<std::uintptr_t>(a_ptr));
		}

		// Check if a demangled type name is a game-relevant object
		// Returns false for STL types, internal implementation classes, etc.
		[[nodiscard]] bool is_game_relevant_type(std::string_view demangled) noexcept
		{
			// Filter out STL and internal implementation types
			// Check for std:: prefix or common STL internal patterns
			if (demangled.starts_with("std::") ||
				demangled.find("std::_") != std::string_view::npos ||
				demangled.starts_with("_Ref_count")) {
				return false;
			}
			if (demangled.starts_with("_")) {
				// Internal implementation classes
				return false;
			}
			// Game-relevant types typically start with known prefixes
			// RE::, BSScript::, hk, Ni, etc.
			return true;
		}

		class Integer
		{
		public:
			Integer(std::size_t a_value) noexcept :
				_value(a_value),
				name_string(a_value >> 63 ?
					fmt::format(fmt::runtime("(size_t) [uint: {} int: {}]"s), _value, static_cast<std::make_signed_t<size_t>>(_value)) :
					fmt::format(fmt::runtime("(size_t) [{}]"s), _value)) {}

			[[nodiscard]] std::string name() const { return name_string; }

		private:
			const std::size_t _value;
			const std::string name_string;
		};

		class Pointer
		{
		public:
			Pointer() noexcept = default;

			Pointer(const void* a_ptr, std::span<const module_pointer> a_modules) noexcept :
				_module(get_module_for_pointer(a_ptr, a_modules))
			{
				if (_module)
					_ptr = a_ptr;
			}

			[[nodiscard]] std::string name() const
			{
				// Check if this address was already introspected as a known object
				{
					std::lock_guard<std::mutex> lock(seen_objects_mutex);
					auto it = seen_objects.find(_ptr);
					if (it != seen_objects.end() && !it->second.result.empty())
						return it->second.result;  // Return the full object information
				}

				if (_module)
				{
					const auto address = reinterpret_cast<std::uintptr_t>(_ptr);
					const auto pdbDetails = Crash::PDB::pdb_details(_module->path(), address - _module->address());
					const auto assembly = _module->assembly((const void*)address);
					std::string result;
					if (!pdbDetails.empty())
						result = fmt::format(
							"(void* -> {}+{:07X}\t{} | {})"sv,
							_module->name(),
							address - _module->address(),
							assembly,
							pdbDetails);
					else
						result = fmt::format(
							"(void* -> {}+{:07X}\t{})"sv,
							_module->name(),
							address - _module->address(),
							assembly);

					// Store in seen_objects to prevent duplicate introspection
					// Mark as NOT a game object (just a void* with module info)
					{
						std::lock_guard<std::mutex> lock(seen_objects_mutex);
						seen_objects.try_emplace(_ptr, result, current_analysis_pos, generate_current_label(_ptr), false);
					}
					return result;
				}
				else
					return "(void*)"s;
			}

		private:
			const Modules::Module* _module{ nullptr };
			const void* _ptr{ nullptr };
		};

		class Polymorphic
		{
		public:
			explicit Polymorphic(std::string_view a_mangled, const void* a_ptr = nullptr) noexcept :
				_mangled{ a_mangled },
				_ptr{ a_ptr }
			{
				// NOLINTNEXTLINE(readability-simplify-subscript-expr)
				assert(_mangled.size() > 1 && _mangled.data()[_mangled.size()] == '\0');
			}

			void set_header(std::string a_header) noexcept { _header = std::move(a_header); }

			[[nodiscard]] std::string demangled_name() const { return Crash::PDB::demangle(std::string{ _mangled }); }

			[[nodiscard]] std::string get_formatted_name() const
			{
				const std::string demangled = demangled_name();
				return _header.empty() ? fmt::format("({}*)"sv, demangled) : _header;
			}

			[[nodiscard]] std::string name() const
			{
				auto result = get_formatted_name();

				// Check if this address was already introspected
				if (_ptr)
				{
					// Determine if this is a game object before acquiring the lock
					const std::string demangled = demangled_name();
					bool is_game_obj = is_game_relevant_type(demangled);

					// Use check-and-reserve pattern
					{
						std::lock_guard<std::mutex> lock(seen_objects_mutex);
						auto [it, inserted] = seen_objects.try_emplace(_ptr, SeenObjectInfo{ result, current_analysis_pos, generate_current_label(_ptr), is_game_obj });

						if (!inserted)
						{
							// If we're at the same position where it was first seen, return the stored result
							if (current_analysis_pos == it->second.first_seen_pos)
								return it->second.result;

							// Object already being processed or completed - return cross-reference
							return fmt::format("{} See {}", result, it->second.first_seen_label);
						}
						// else: we successfully stored this object, return the result
					}
				}

				return result;
			}

		private:
			std::string_view _mangled;
			const void* _ptr{ nullptr };
			std::string _header;
		};

		class F4Polymorphic
		{
		public:
			F4Polymorphic(
				std::string_view a_mangled,
				const RE::RTTI::CompleteObjectLocator* a_col,
				const void* a_ptr) noexcept :
				_poly{ a_mangled, a_ptr },
				_col{ a_col },
				_ptr{ a_ptr }
			{
				assert(_col != nullptr);
				assert(_ptr != nullptr);
			}

			void set_header(std::string a_header) noexcept { _poly.set_header(std::move(a_header)); }
			[[nodiscard]] std::string demangled_name() const { return _poly.demangled_name(); }

			[[nodiscard]] std::string name() const
			{
				std::size_t reserved_pos = 0;
				bool was_inserted = false;

				// Use check-and-reserve pattern to prevent re-entrancy
				{
					std::lock_guard<std::mutex> lock(seen_objects_mutex);
					auto [it, inserted] = seen_objects.try_emplace(_ptr, SeenObjectInfo{ "", current_analysis_pos, generate_current_label(_ptr), true });
					was_inserted = inserted;
					reserved_pos = current_analysis_pos;

					if (!inserted)
					{
						// Object already exists (either being processed or completed)

						// If we're at the same position where it was first seen, return the stored result
						// (This happens on the second analysis pass for printing)
						if (current_analysis_pos == it->second.first_seen_pos && !it->second.result.empty())
							return it->second.result;

						// Different position - generate cross-reference
						auto poly_name = _poly.get_formatted_name();

						if (it->second.result.empty())
						{
							// Being processed by another thread or recursively - return placeholder
							return fmt::format("({}) See {}", poly_name, it->second.first_seen_label);
						}
						else
						{
							// Already completed - return cross-reference
							return fmt::format("{} See {}", poly_name, it->second.first_seen_label);
						}
					}
					// else: we successfully reserved this slot, continue with introspection
				}

				auto result = _poly.get_formatted_name();
				F4::filter_results xInfo;

				const auto moduleBase = REX::FModule::GetExecutingModule().GetBaseAddress();
				const auto hierarchy = _col->classDescriptor.get();
				const std::span bases(
					reinterpret_cast<std::uint32_t*>(hierarchy->baseClassArray.offset() + moduleBase),
					hierarchy->numBaseClasses);
				for (const auto rva : bases)
				{
					const auto base = reinterpret_cast<RE::RTTI::BaseClassDescriptor*>(rva + moduleBase);
					const auto it = FILTERS.find(base->typeDescriptor->raw_name());
					if (it != FILTERS.end())
					{
						const auto root = REX::ADJUST_POINTER<void>(_ptr, -static_cast<std::ptrdiff_t>(_col->offset));
						const auto target = REX::ADJUST_POINTER<void>(root, static_cast<std::ptrdiff_t>(base->pmd.mDisp));
						it->second(xInfo, target, 0);
					}
					else
					{
						// Demangle the type name for better readability using the improved PDB demangler
						const char* mangled_name = base->typeDescriptor->raw_name();
						if (mangled_name && mangled_name[0] != '\0') {
							std::string demangled_info = Crash::PDB::demangle(std::string(mangled_name));
							LOG::INFO("Found unhandled type:\t{}\t{} [{}]"sv, result, mangled_name, demangled_info);
						}
						else
							LOG::INFO("Found unhandled type:\t{}\t<null>"sv, result);
					}
				}

				// Post-process filters to reduce verbosity and improve header
				std::string rootFile, rootName, rootFormID, rootFormType, rootFlags;
				std::size_t rootFileIdx = std::string::npos, rootNameIdx = std::string::npos,
					rootFormIDIdx = std::string::npos, rootFormTypeIdx = std::string::npos, rootFlagsIdx = std::string::npos;

				for (std::size_t i = 0; i < xInfo.size(); ++i)
				{
					const auto& key = xInfo[i].first;
					const auto& val = xInfo[i].second;
					// Only match root-level keys (no leading tabs) to avoid matching nested "Full Name", "Skeleton Name", etc.
					if (key == "File")
					{
						rootFile = val;
						rootFileIdx = i;
					}
					else if (key == "Name")
					{
						rootName = val;
						rootNameIdx = i;
					}
					else if (key == "FormID")
					{
						rootFormID = val;
						rootFormIDIdx = i;
					}
					else if (key == "FormType")
					{
						rootFormType = val;
						rootFormTypeIdx = i;
					}
					else if (key == "Flags")
					{
						rootFlags = val;
						rootFlagsIdx = i;
					}
				}

				// Append to header
				if (!rootName.empty())
					result += fmt::format(" {}"sv, rootName);
				if (!rootFormID.empty())
					result += fmt::format(" [{}]"sv, rootFormID);
				if (!rootFile.empty())
					result += fmt::format(" ({})"sv, rootFile);

				// Mark for removal
				std::vector<bool> remove(xInfo.size(), false);
				if (rootFileIdx != std::string::npos)
					remove[rootFileIdx] = true;
				if (rootNameIdx != std::string::npos)
					remove[rootNameIdx] = true;
				if (rootFormIDIdx != std::string::npos)
					remove[rootFormIDIdx] = true;
				if (rootFormTypeIdx != std::string::npos)
					remove[rootFormTypeIdx] = true;
				if (rootFlagsIdx != std::string::npos)
					remove[rootFlagsIdx] = true;

				// Remove duplicates (nested objects matching root)
				for (std::size_t i = 0; i < xInfo.size(); ++i) {
					if (remove[i])
						continue;
					const auto& key = xInfo[i].first;
					const auto& val = xInfo[i].second;

					// Match any depth for duplicate removal (starts with tab or exact match)
					if ((key == "File" || key.find("\tFile") == 0) && val == rootFile)
						remove[i] = true;
					if ((key == "Name" || key.find("\tName") == 0) && val == rootName)
						remove[i] = true;
				}

				if (!xInfo.empty())
				{
					for (std::size_t i = 0; i < xInfo.size(); ++i)
					{
						if (!remove[i])
						{
							result += fmt::format(
								"\n\t\t{}: {}"sv,
								xInfo[i].first,
								xInfo[i].second);
						}
					}
				}

				// Check if this is a game-relevant type (filter out STL types)
				// Extract the type name from result: "(TypeName*)"
				std::string_view result_view(result);
				std::size_t start = result_view.find('(');
				std::size_t end = result_view.find("*)");
				bool is_game_obj = true;  // Default to true for F4 types

				if (start != std::string_view::npos && end != std::string_view::npos && end > start)
				{
					std::string_view type_name = result_view.substr(start + 1, end - start - 1);
					is_game_obj = is_game_relevant_type(type_name);
				}

				// Update the reserved slot with the complete result
				{
					std::lock_guard<std::mutex> lock(seen_objects_mutex);
					auto it = seen_objects.find(_ptr);
					if (it != seen_objects.end())
					{
						it->second.result = result;
						it->second.is_game_object = is_game_obj;
					}
				}

				return result;
			}

		private:
			static constexpr auto FILTERS = frozen::make_map(
			{
				std::make_pair(".?AULooseFileStreamBase@?A0xaf4cad8a@BSResource@@"sv, F4::BSResource::LooseFileStreamBase::filter),
				std::make_pair(".?AVBSShaderProperty@@"sv, F4::BSShaderProperty::filter),
				std::make_pair(".?AVCodeTasklet@Internal@BSScript@@"sv, F4::CodeTasklet::filter),
				std::make_pair(".?AVCharacter@@"sv, F4::TESForm<RE::PlayerCharacter>::filter),
				std::make_pair(".?AVExtraTextDisplayData@@"sv, F4::ExtraTextDisplayData::filter),
				std::make_pair(".?AVNativeFunctionBase@NF_util@BSScript@@"sv, F4::BSScript::NF_util::NativeFunctionBase::filter),
				std::make_pair(".?AVNiAVObject@@"sv, F4::NiAVObject::filter),
				std::make_pair(".?AVNiObjectNET@@"sv, F4::NiObjectNET::filter),
				std::make_pair(".?AVNiStream@@"sv, F4::NiStream::filter),
				std::make_pair(".?AVNiTexture@@"sv, F4::NiTexture::filter),
				std::make_pair(".?AVObjectTypeInfo@BSScript@@"sv, F4::BSScript::ObjectTypeInfo::filter),
				std::make_pair(".?AVPlayerCharacter@@"sv, F4::TESForm<RE::PlayerCharacter>::filter),
				std::make_pair(".?AVTESFaction@@"sv, F4::TESForm<RE::TESFaction>::filter),
				std::make_pair(".?AVTESForm@@"sv, F4::TESForm<RE::TESForm>::filter),
				std::make_pair(".?AVTESFullName@@"sv, F4::TESFullName::filter),
				std::make_pair(".?AVTESNPC@@"sv, F4::TESForm<RE::TESNPC>::filter),
				std::make_pair(".?AVTESObjectCELL@@"sv, F4::TESForm<RE::TESObjectCELL>::filter),
				std::make_pair(".?AVTESObjectREFR@@"sv, F4::TESObjectREFR::filter),
				std::make_pair(".?AVTESQuest@@"sv, F4::TESQuest::filter),
			});

			Polymorphic _poly;
			const RE::RTTI::CompleteObjectLocator* _col{ nullptr };
			const void* _ptr{ nullptr };
		};

		class String
		{
		public:
			String(std::string_view a_str) noexcept : _str(a_str) {}

			[[nodiscard]] std::string name() const
			{
				return fmt::format("(char*) \"{}\""sv, _str);
			}

		private:
			std::string_view _str;
		};

		class HeapPointer
		{
		public:
			HeapPointer(const void* a_ptr, const Heap::HeapInfo& a_info) noexcept : _ptr(a_ptr), _info(a_info) {}

			[[nodiscard]] std::string name() const
			{
				return fmt::format("(void*) 0x{:012X} [Heap: {}]"sv,
					reinterpret_cast<std::uintptr_t>(_ptr),
					Heap::format_heap_info(_info));
			}

		private:
			const void* _ptr;
			Heap::HeapInfo _info;
		};

		using analysis_result = std::variant<
			Integer,
			Pointer,
			Polymorphic,
			F4Polymorphic,
			String,
			HeapPointer>;

		template <class T, class... Args>
		[[nodiscard]] analysis_result make_result(Args&&... a_args) noexcept(std::is_nothrow_constructible_v<T, Args...>)
		{
			return analysis_result(std::in_place_type_t<T>{}, std::forward<Args>(a_args)...);
		}

		[[nodiscard]] auto analyze_polymorphic(
			void* a_ptr,
			std::span<const module_pointer> a_modules) noexcept
			-> std::optional<analysis_result>
		{
			try
			{
				const auto vtable = *reinterpret_cast<void**>(a_ptr);
				const auto mod = get_module_for_pointer(vtable, a_modules);
				if (!mod || !mod->in_rdata_range(vtable))
					return std::nullopt;

				const auto col =
					*reinterpret_cast<RE::RTTI::CompleteObjectLocator**>(
						reinterpret_cast<std::size_t*>(vtable) - 1);
				if (mod != get_module_for_pointer(col, a_modules) || !mod->in_rdata_range(col))
					return std::nullopt;

				const auto typeDesc =
					reinterpret_cast<RE::RTTI::TypeDescriptor*>(
						mod->address() + col->typeDescriptor.offset());
				if (mod != get_module_for_pointer(typeDesc, a_modules) || !mod->in_data_range(typeDesc))
					return std::nullopt;

				if (*reinterpret_cast<const void**>(typeDesc) != mod->type_info())
					return std::nullopt;

				if (_stricmp(mod->name().data(), util::module_name().c_str()) == 0)
					return make_result<F4Polymorphic>(typeDesc->raw_name(), col, a_ptr);
				else
					return make_result<Polymorphic>(typeDesc->raw_name());
			}
			catch (...)
			{
				return std::nullopt;
			}
		}

		[[nodiscard]] auto analyze_string(void* a_ptr) noexcept -> std::optional<analysis_result>
		{
			try
			{
				const auto printable = [](char a_ch) noexcept
				{
					if (' ' <= a_ch && a_ch <= '~')
						return true;
					else
					{
						switch (a_ch)
						{
						case '\t':
						case '\n':
							return true;
						default:
							return false;
						}
					}
				};

				const auto str = static_cast<const char*>(a_ptr);
				constexpr std::size_t max = 1000;
				std::size_t len = 0;
				for (; len < max && str[len] != '\0'; ++len)
				{
					if (!printable(str[len]))
						return std::nullopt;
				}

				if (len == 0 || len >= max)
					return std::nullopt;

				return make_result<String>(std::string_view{ str, len });
			}
			catch (...)
			{
				return std::nullopt;
			}
		}

		[[nodiscard]] auto analyze_pointer(
			void* a_ptr,
			std::span<const module_pointer> a_modules) noexcept
			-> analysis_result
		{
			if (auto poly = analyze_polymorphic(a_ptr, a_modules); poly)
				return *std::move(poly);

			if (auto str = analyze_string(a_ptr); str)
				return *std::move(str);

			// Check if pointer is in a heap allocation
			// Wrap in try-catch since analyze_heap_pointer can throw (e.g., OOM during crash)
			try
			{
				if (auto heap_info = Heap::analyze_heap_pointer(a_ptr); heap_info)
					return make_result<HeapPointer>(a_ptr, *heap_info);
			}
			catch (...)
			{
				// Swallow exception and fall back to basic pointer analysis
				// (better to lose heap metadata than the entire crash log)
			}

			return make_result<Pointer>(a_ptr, a_modules);
		}

		[[nodiscard]] auto analyze_integer(
			std::size_t a_value,
			std::span<const module_pointer> a_modules) noexcept
			-> analysis_result
		{
			try {
				if (a_value != 0) {
					*reinterpret_cast<const volatile std::byte*>(a_value);
					return analyze_pointer(reinterpret_cast<void*>(a_value), a_modules);
				}
			}
			catch (...) {}

			return make_result<Integer>(a_value);
		}
	}

	void reset_analysis_state() noexcept
	{
		std::lock_guard<std::mutex> lock(detail::seen_objects_mutex);
		detail::seen_objects.clear();
		detail::backfill_logged_this_crash = false;
		detail::total_backfill_count = 0;
	}

	std::vector<std::string> analyze_data(
		std::span<const std::size_t> a_data,
		std::span<const module_pointer> a_modules,
		std::function<std::string(size_t)> a_label_generator)
	{
		detail::label_generator = a_label_generator;
		std::vector<std::string> results;
		results.resize(a_data.size());
		std::for_each(
			std::execution::par_unseq,
			a_data.begin(),
			a_data.end(),
			[&](auto& a_val)
			{
				const auto pos = std::addressof(a_val) - a_data.data();
				detail::current_analysis_pos = static_cast<std::size_t>(pos);
				const auto result = detail::analyze_integer(a_val, a_modules);
				results[pos] = std::visit(
					[](const auto& a_analysis) { return a_analysis.name(); },
					result);
			});
		return results;
	}
}

void Crash::Introspection::backfill_void_pointers(std::vector<std::string>& a_results, std::span<const std::size_t> a_addresses)
{
	assert(a_results.size() == a_addresses.size());

	for (std::size_t i = 0; i < a_results.size(); ++i)
	{
		auto& result = a_results[i];
		std::size_t addr = a_addresses[i];

		// Only process entries that are still void* pointers (not already replaced)
		if (result.starts_with("(void*"))
		{
			// Check if this address points to a known object
			auto it = detail::seen_objects.find(reinterpret_cast<const void*>(addr));
			if (it != detail::seen_objects.end())
			{
				// Replace with full object information
				result = it->second.result;
				++detail::total_backfill_count;
			}
		}
	}

	// Log the backfill statistics (only once per crash)
	if (!detail::backfill_logged_this_crash && detail::total_backfill_count > 0)
	{
		LOG::INFO("Backfilled {} void* pointers with known object information across all analysis", detail::total_backfill_count);
		detail::backfill_logged_this_crash = true;
	}
}

bool Crash::Introspection::was_introspected(const void* a_ptr) noexcept
{
	// Return true ONLY if the object is a game object (polymorphic type)
	// Exclude void* pointers with module info (those are not game objects)
	std::lock_guard<std::mutex> lock(detail::seen_objects_mutex);
	auto it = detail::seen_objects.find(a_ptr);
	return it != detail::seen_objects.end() && it->second.is_game_object;
}
