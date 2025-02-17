#pragma once
#include <exception>
#include <string>
#include <string_view>
#include <vector>
#include <tuple>
#include <type_traits>
#include <format>


class ContextualException : public std::exception
{
public:
	// Constructors & Deconstructors
	ContextualException();
	ContextualException(std::string message);
	ContextualException(std::string message, std::string context);
	~ContextualException();

	ContextualException(const ContextualException&) = delete;
	ContextualException(ContextualException&&) = default;
	ContextualException& operator=(const ContextualException&) = delete;
	ContextualException& operator=(ContextualException&&) = default;

	// Accessors
	[[nodiscard]] char const* what() const noexcept override; // Note: Not threadsafe

private:
	std::string message;
	std::vector<std::string> context;

	mutable std::string formatted_message;
};

template<typename... TArgs>
class ContextArea
{
public:
	ContextArea(std::string context_fmt, TArgs&&... args);
	ContextArea(std::string_view context_fmt, TArgs&&... args);
	~ContextArea();

private:
	// TODO: This can be optimized out with another template param
	std::string context_fmt_owned;
	std::string_view context_fmt;
	std::tuple<TArgs...> args;
};


// Implementation
// ============================================================================

namespace Detail
{
	void context_area_add_context_current_thread(std::string&& context_point);
}

template<typename... TArgs>
ContextArea<TArgs...>::ContextArea(std::string context_fmt, TArgs&&... args)
	: context_fmt_owned(std::move(context_fmt))
	, context_fmt(context_fmt_owned)
	, args(std::forward<TArgs>(args)...)
{
}

template<typename... TArgs>
ContextArea<TArgs...>::ContextArea(std::string_view context_fmt, TArgs&&... args)
	: context_fmt(context_fmt)
	, args(std::forward<TArgs>(args)...)
{
}

template<typename... TArgs>
ContextArea<TArgs...>::~ContextArea()
{
	if (std::uncaught_exceptions() > 0) [[unlikely]]
	{
		std::string formatted_context;

		if constexpr (sizeof...(TArgs) == 0)
		{
			formatted_context = std::string{ context_fmt };
		}
		else 
		{
			auto format_with_tuple = []<typename... TArgs, size_t... I>(std::string_view fmt, std::tuple<TArgs...>&args, std::index_sequence<I...>)
			{
				return std::format(fmt, std::get<I>(args)...);
			};
			formatted_context = format_with_tuple(context_fmt, args, std::index_sequence_for<TArgs...>{});
		}
		
		Detail::context_area_add_context_current_thread(std::move(formatted_context));
	}
}