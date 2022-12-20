#include "CodeGenerator.h"

#include "ContextualException.h"

#include <format>
#include <cassert>
#include <cctype>
#include <algorithm>

#include <iostream>

#include "ifc/Declaration.h"
#include "ifc/File.h"
#include "ifc/Expression.h"
#include "ifc/Type.h"
#include "ifc/Name.h"


CodeGenerator::CodeGenerator(ifc::File& file)
	: file(file)
{
	code.reserve(1024);
}

void CodeGenerator::write_cpp_file(std::ostream& out)
{
	if (file.header().unit.sort() != ifc::UnitSort::Primary) // For now
	{
		throw ContextualException("Currently the tool only supports primary module fragments (originating from a .ixx file from MSVC for example).");
	}

	auto unit_index = file.header().unit.index;
	const auto module_name = file.get_string(ifc::TextOffset{ unit_index });
	
	scan(file.global_scope());
	
	out << std::format(
R"(// ================================================================================
//                      AUTO GENERATED REFLECTION DATA FILE 
//                    Generated by: NeatReflectionCodeGen.exe
// 
//       Don't modify this file, it will be overwritten when a change is made.
// ================================================================================

#include "Neat/Reflection.h"
#include "Neat/TemplateTypeId.h"

import {0};


namespace Neat
{{
	static void reflect_private_members()
	{{
{1}
	}}

	namespace Detail
	{{
		struct Register{{ Register(){{ Neat::reflect_private_members(); }} }};
		static Register neat_reflection_data_initialiser{{ }};
	}}
}})", module_name, code);

	out.flush();
}

void CodeGenerator::scan(ifc::ScopeDescriptor scope_desc)
{
	auto declarations = ifc::get_declarations(file, scope_desc);
	for (auto& declaration : declarations)
	{
		scan(declaration.index);
	}
}

void CodeGenerator::scan(ifc::DeclIndex decl)
{
	switch (decl.sort())
	{
	case ifc::DeclSort::Scope:
		scan(ifc::get_scope(file, decl));
		break;
	}
}

void CodeGenerator::scan(const ifc::ScopeDeclaration& scope_decl)
{
	switch (ifc::get_kind(scope_decl, file))
	{
	case ifc::TypeBasis::Class:
	case ifc::TypeBasis::Struct:
		render(get_user_type_name(file, scope_decl.name), scope_decl);
		break;
	case ifc::TypeBasis::Union:
		// TODO: Implement at some point
		break;
	case ifc::TypeBasis::Namespace:
		scan(file.scope_descriptors()[scope_decl.initializer]);
		break;
	}
}

void CodeGenerator::render(std::string_view type_name, const ifc::ScopeDeclaration& scope_decl)
{
	if ((uint8_t)scope_decl.specifiers & (uint8_t)ifc::BasicSpecifiers::NonExported)
	{
		return;
	}

	const bool is_class = (ifc::get_kind(scope_decl, file) == ifc::TypeBasis::Class);
	const auto default_access = (is_class ? Neat::Access::Private : Neat::Access::Public);

	auto var_name = to_snake_case(type_name) + '_';
	auto scope_descriptor = file.scope_descriptors()[scope_decl.initializer];
	const bool reflect_privates = should_reflect_private_members(scope_descriptor, default_access);
	auto [fields, methods] = render_members(type_name, var_name, scope_descriptor, reflect_privates);
	auto bases = render_bases(scope_decl);

	code += std::format(R"(add_type({{ "{1}", get_id<{1}>(),
	{{ {2} }},
	{{ {3} }},
	{{ {4} }}
}});
)", var_name, type_name, bases, fields, methods);
}

CodeGenerator::TypeMembers CodeGenerator::render_members(std::string_view type_name, std::string_view type_variable, ifc::ScopeDescriptor scope_desc, bool reflect_private_members)
{
	std::string fields;
	std::string methods;

	auto declarations = ifc::get_declarations(file, scope_desc);
	for (auto& decl : declarations)
	{
		switch (decl.index.sort())
		{
		case ifc::DeclSort::Field:
		{
			const auto& field = file.fields()[decl.index];
			const auto type = render_full_typename(field.type);
			const auto name = file.get_string(field.name);
			const auto access = render_as_neat_access_enum(field.access, "Access::...");

			if (field.access == ifc::Access::Public || reflect_private_members)
			{
				fields += std::format(R"(Field::create<{0}, {1}, &{0}::{2}>("{2}", {3}), )", type_name, type, name, access);
			}
			break;
		}
		case ifc::DeclSort::Method:
		{
			const auto& method = file.methods()[decl.index];
			assert(method.type.sort() == ifc::TypeSort::Method);
			const auto& method_type = file.method_types()[method.type];
			const auto return_type = render_full_typename(method_type.target);
			auto param_types = std::string{ "" };
			if (!method_type.source.is_null())
			{
				param_types = ", " + render_full_typename(method_type.source);
			}
			const auto name = get_user_type_name(file, method.name);
			const auto access = render_as_neat_access_enum(method.access);

			if (method.access == ifc::Access::Public || reflect_private_members)
			{
				methods += std::format(R"(Method::create<&{0}::{3}, {0}, {1}{2}>("{3}", {4}), )", type_name, return_type, param_types, name, access);
			}
			break;
		}
		}
	}
	return { fields, methods };
}

std::string CodeGenerator::render_bases(const ifc::ScopeDeclaration& scope_decl)
{
	// Otherwise struct
	const bool is_class = (ifc::get_kind(scope_decl, file) == ifc::TypeBasis::Class);
	const auto default_access = (is_class ? "private" : "public");

	auto base_index = scope_decl.base;
	if (base_index.is_null())
	{
		return "";
	}

	const auto render_base = [this, default_access] (const ifc::BaseType& base_type) -> std::string
	{
		auto access_string = render_as_neat_access_enum(base_type.access, default_access);
		auto type_name = render_full_typename(base_type.type);
		return std::format(R"(BaseClass{{ get_id<{0}>(), {1} }}, )", type_name, access_string);
	};

	switch (base_index.sort())
	{
	case ifc::TypeSort::Base: 
		return render_base(file.base_types()[base_index]);
	case ifc::TypeSort::Tuple:
		{
			auto& tuple_type = file.tuple_types()[base_index];

			std::string rendered;
			rendered.reserve(ifc::raw_count(tuple_type.cardinality) * 16);

			for (auto& type : file.type_heap().slice(tuple_type))
			{
				if (type.sort() == ifc::TypeSort::Base)
				{
					rendered += render_base(file.base_types()[type]);
				}
				else
				{
					assert(false && "unexpected base type");
				}
			}

			return rendered;
		}
		break;
	default:
		// TODO: Throw exception
		assert(false && "Unexpected base class type sort");
		return "";
	}
}

std::string CodeGenerator::render_full_typename(ifc::TypeIndex type_index)
{
	switch (type_index.sort())
	{
	case ifc::TypeSort::Fundamental:
		return render_full_typename(file.fundamental_types()[type_index]);
	case ifc::TypeSort::Designated:
		return render_refered_declaration(file.designated_types()[type_index].decl);
	case ifc::TypeSort::Pointer:
		return render_full_typename(file.pointer_types()[type_index].pointee) + "*";
	case ifc::TypeSort::LvalueReference:
		return render_full_typename(file.lvalue_references()[type_index].referee) + "&";
	case ifc::TypeSort::RvalueReference:
		return render_full_typename(file.rvalue_references()[type_index].referee) + "&&";
	case ifc::TypeSort::Qualified:
		return render_full_typename(file.qualified_types()[type_index].unqualified) +
			render(file.qualified_types()[type_index].qualifiers);
	case ifc::TypeSort::Base:
		// render only the typename, no access modifiers or specifiers
		return render_full_typename(file.base_types()[type_index].type);
	case ifc::TypeSort::Placeholder:
		if (auto& elaborated_type = file.placeholder_types()[type_index].elaboration; !elaborated_type.is_null())
		{
			return render_full_typename(elaborated_type);
		}
		assert(false && "IFC doesn't contain deduced type");
		return "PLACEHOLDER_TYPE";
	case ifc::TypeSort::Tuple:
		return render_full_typename(file.tuple_types()[type_index]);

		// Currently unsupported
	case ifc::TypeSort::Expansion: // variadic pack expansion (...)
	case ifc::TypeSort::Function: // U (*)(Args...);
	case ifc::TypeSort::PointerToMember: // U (T::*);
	case ifc::TypeSort::Method: // U (T::*)(Args...);
	// case ifc::TypeSort::Array: // T t[N]; Not implemented yet in ifc-reader
	// case ifc::TypeSort::Typename: // typename T::dependant_type; Not implementet yet in ifc-reader
	case ifc::TypeSort::Decltype: // Seems to complicated to support
	case ifc::TypeSort::Forall: // Template declaration. Not used yet in MSVC
	case ifc::TypeSort::Unaligned: // __unaligned T; Partition not implemented yet in ifc-reader

		// Not planned to be supported
	case ifc::TypeSort::VendorExtension:
	case ifc::TypeSort::Tor: // Compiler generated constructor
	case ifc::TypeSort::Syntactic: // type expressed at the C++ source-level as a type-id. Typical examples include a template - id designating a specialization.
	case ifc::TypeSort::SyntaxTree: // General parse tree representation. Seems to complicated to support

	default:
		//assert(false && "Not supported yet");
		return "UNSUPPORTED_TYPE_" + std::to_string((uint32_t)type_index.sort());
	}
}

std::string CodeGenerator::render_full_typename(const ifc::FundamentalType& type)
{
	std::string rendered;
	rendered.reserve(16);

	if (type.sign == ifc::TypeSign::Unsigned)
	{
		rendered += "unsigned ";
	}

	switch (type.precision)
	{
	case ifc::TypePrecision::Default:
		break;
	case ifc::TypePrecision::Short:
		assert(type.basis == ifc::TypeBasis::Int);
		return "short";
	case ifc::TypePrecision::Long:
		rendered += "long ";
		break;
	case ifc::TypePrecision::Bit64:
		assert(type.basis == ifc::TypeBasis::Int);
		return "long long";
	case ifc::TypePrecision::Bit8:
		if (type.basis == ifc::TypeBasis::Char)
		{
			return "char8_t";
		}
	case ifc::TypePrecision::Bit16:
		if (type.basis == ifc::TypeBasis::Char)
		{
			return "char16_t";
		}
	case ifc::TypePrecision::Bit32:
		if (type.basis == ifc::TypeBasis::Char)
		{
			return "char32_t";
		}
	case ifc::TypePrecision::Bit128:
		rendered += std::format("Unsupported Bitness '{0}' ", 
			static_cast<int>(type.precision));
		break;
	}

	switch (type.basis)
	{
	case ifc::TypeBasis::Void:
		rendered += "void";
		break;
	case ifc::TypeBasis::Bool:
		rendered += "bool";
		break;
	case ifc::TypeBasis::Char:
		rendered += "char";
		break;
	case ifc::TypeBasis::Wchar_t:
		rendered += "wchar_t";
		break;
	case ifc::TypeBasis::Int:
		rendered += "int";
		break;
	case ifc::TypeBasis::Float:
		rendered += "float";
		break;
	case ifc::TypeBasis::Double:
		rendered += "double";
		break;
	default:
		rendered += std::format("fundamental type ({0})", 
			static_cast<int>(type.basis));
		break;
	}

	return rendered;
}


std::string CodeGenerator::render_full_typename(const ifc::TupleType& types)
{
	std::string rendered;
	rendered.reserve(ifc::raw_count(types.cardinality) * 8); // Preallocate a reasonable amount

	bool first = true;
	for (auto& type : file.type_heap().slice(types))
	{
		if (!first)
		{
			rendered += ", ";
		}

		rendered += render_full_typename(type);

		first = false;
	}

	return rendered;
}

std::string CodeGenerator::render_refered_declaration(const ifc::DeclIndex& decl_index)
{
	switch (const auto kind = decl_index.sort())
	{
	case ifc::DeclSort::Parameter:
	{
		ifc::ParameterDeclaration const& param = file.parameters()[decl_index];
		return file.get_string(param.name);
	}
	break;
	case ifc::DeclSort::Scope:
	{
		ifc::ScopeDeclaration const& scope = get_scope(file, decl_index);
		return std::string{ get_user_type_name(file, scope.name) };
	}
	break;
	case ifc::DeclSort::Template:
	{
		ifc::TemplateDeclaration const& template_declaration = file.template_declarations()[decl_index];
		return std::string{ get_user_type_name(file, template_declaration.name) };
	}
	break;
	case ifc::DeclSort::Function:
	{
		ifc::FunctionDeclaration const& function = file.functions()[decl_index];
		return std::string{ get_user_type_name(file, function.name) };
	}
	break;
	//case ifc::DeclSort::Reference:
	//	return std::string{ get_user_type_name(file, file.decl_references()[decl_index]) };
	//	break;
	case ifc::DeclSort::Enumeration:
	{
		auto const& enumeration = file.enumerations()[decl_index];
		return file.get_string(enumeration.name);
	}
	break;
	default:
		assert(false && "Unsupported declsort");
		return "Unsupported DeclSort '" + std::to_string(static_cast<int>(kind)) + "'";
	}
}

std::string CodeGenerator::render(ifc::Qualifiers qualifiers)
{
	using namespace std::string_view_literals;

	std::string rendered;
	rendered.reserve("const"sv.size() + "volatile"sv.size());
	
	if (ifc::has_qualifier(qualifiers, ifc::Qualifiers::Const))
	{
		rendered += "const ";
	}
	if (ifc::has_qualifier(qualifiers, ifc::Qualifiers::Volatile))
	{
		rendered += "volatile ";
	}
	if (ifc::has_qualifier(qualifiers, ifc::Qualifiers::Restrict))
	{
		// Ignored
	}

	return rendered;
}

std::string CodeGenerator::render_as_neat_access_enum(ifc::Access access, std::string_view value_for_none)
{
	switch (access)
	{
	case ifc::Access::None: return std::string{ value_for_none };
	case ifc::Access::Private: return "Neat::Access::Private";
	case ifc::Access::Protected: return "Neat::Access::Protected";
	case ifc::Access::Public: return "Neat::Access::Public";
	}

	throw ContextualException("Invalid access value.", 
		std::format("Expected 0 to 3 (inclusive). While {0} was given.", static_cast<uint8_t>(access)));
}

std::string CodeGenerator::render_neat_access_enum(Neat::Access access)
{
	switch (access)
	{
	case Neat::Access::Private: return "Neat::Access::Private";
	case Neat::Access::Protected: return "Neat::Access::Protected";
	case Neat::Access::Public: return "Neat::Access::Public";
	default:
		throw ContextualException("Invalid access value.",
			std::format("Expected 0 to 3 (inclusive). While {0} was given.", static_cast<uint8_t>(access)));
	}
}

bool CodeGenerator::should_reflect_private_members(ifc::ScopeDescriptor scope_desc, Neat::Access default_access) const
{
	std::cout << "should reflect?";
	auto declarations = ifc::get_declarations(file, scope_desc);
	for (auto& decl : declarations)
	{
		if(decl.index.sort() == ifc::DeclSort::Friend)
		{
			const auto& friend_decl = file.friend_declarations()[decl.index];
			if (friend_decl.entity.sort() == ifc::ExprSort::NamedDecl)
			{
				std::cout << "is declsort " << (int)friend_decl.entity.sort() << '\n';
				return true;
			}
			else {
				std::cout << "different declsort " << (int)friend_decl.entity.sort() << '\n';
			}
		}
	}

	return false;
}

std::string_view get_user_type_name(const ifc::File& file, ifc::NameIndex name)
{
	assert(name.sort() == ifc::NameSort::Identifier);
	return { file.get_string(ifc::TextOffset{ name.index }) };
}

std::string replace_all_copy(std::string str, std::string_view target, std::string_view replacement)
{
	size_t start_pos = 0;
	while ((start_pos = str.find(target, start_pos)) != std::string::npos) {
		str.replace(start_pos, target.length(), replacement);
		start_pos += replacement.length(); // Handles case where 'to' is a substring of 'from'
	}
	return str;
}

std::string to_snake_case(std::string_view type_name)
{
	std::string snake_case;
	snake_case.reserve(type_name.size() + 10);

	bool previous_uppercase = true;
	for (auto c : type_name)
	{
		if (!previous_uppercase && std::isupper(c))
		{
			snake_case.push_back('_');
		}

		if (std::isalnum(c))
		{
			snake_case.push_back(std::tolower(c));
		}
		else 
		{
			snake_case.push_back('_'); // Fallback
		}

		previous_uppercase = std::isupper(c);
	}

	return snake_case;
}