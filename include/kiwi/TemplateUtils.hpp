#pragma once
#include <array>

namespace kiwi
{
	namespace tp
	{
		template<size_t a, size_t b>
		struct gcd
		{
			static constexpr size_t value = gcd<b, a% b>::value;
		};

		template<size_t a>
		struct gcd<a, 0>
		{
			static constexpr size_t value = a;
		};

		template<size_t a, size_t b>
		struct lcm
		{
			static constexpr size_t value = a * b / gcd<a, b>::value;
		};

		template<class _T> using Invoke = typename _T::type;

		template<ptrdiff_t...> struct seq { using type = seq; };

		template<class _S1, class _S2> struct concat;

		template<ptrdiff_t... _i1, ptrdiff_t... _i2>
		struct concat<seq<_i1...>, seq<_i2...>>
			: seq<_i1..., (sizeof...(_i1) + _i2)...> {};

		template<class _S1, class _S2>
		using Concat = Invoke<concat<_S1, _S2>>;

		template<size_t _n> struct gen_seq;
		template<size_t _n> using GenSeq = Invoke<gen_seq<_n>>;

		template<size_t _n>
		struct gen_seq : Concat<GenSeq<_n / 2>, GenSeq<_n - _n / 2>> {};

		template<> struct gen_seq<0> : seq<> {};
		template<> struct gen_seq<1> : seq<0> {};

		template<class Ty>
		struct SeqSize;

		template<ptrdiff_t ..._i>
		struct SeqSize<seq<_i...>>
		{
			static constexpr size_t value = sizeof...(_i);
		};

		template<size_t n, class Seq, ptrdiff_t ..._j>
		struct slice;

		template<size_t n, class Seq, ptrdiff_t ..._j>
		using Slice = Invoke<slice<n, Seq, _j...>>;

		template<size_t n, ptrdiff_t first, ptrdiff_t ..._i, ptrdiff_t ..._j>
		struct slice<n, seq<first, _i...>, _j...>
		{
			using type = Slice<n - 1, seq<_i...>, _j..., first>;
		};

		template<ptrdiff_t first, ptrdiff_t ..._i, ptrdiff_t ..._j>
		struct slice<0, seq<first, _i...>, _j...>
		{
			using type = seq<_j...>;
		};

		template<ptrdiff_t ..._j>
		struct slice<0, seq<>, _j...>
		{
			using type = seq<_j...>;
		};

		template<size_t n, class Seq, ptrdiff_t ...j>
		struct get;

		template<size_t n, ptrdiff_t first, ptrdiff_t ..._i>
		struct get<n, seq<first, _i...>> : get<n - 1, seq<_i...>>
		{
		};

		template<ptrdiff_t first, ptrdiff_t ..._i>
		struct get<0, seq<first, _i...>> : std::integral_constant<ptrdiff_t, first>
		{
		};

		template<>
		struct get<0, seq<>>
		{
		};
	}
}