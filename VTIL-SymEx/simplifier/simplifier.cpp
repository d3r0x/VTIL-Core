// Copyright (c) 2020 Can Boluk and contributors of the VTIL Project   
// All rights reserved.   
//    
// Redistribution and use in source and binary forms, with or without   
// modification, are permitted provided that the following conditions are met: 
//    
// 1. Redistributions of source code must retain the above copyright notice,   
//    this list of conditions and the following disclaimer.   
// 2. Redistributions in binary form must reproduce the above copyright   
//    notice, this list of conditions and the following disclaimer in the   
//    documentation and/or other materials provided with the distribution.   
// 3. Neither the name of VTIL Project nor the names of its contributors
//    may be used to endorse or promote products derived from this software 
//    without specific prior written permission.   
//    
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" 
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE   
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE  
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE   
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR   
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF   
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS   
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN   
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)   
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE  
// POSSIBILITY OF SUCH DAMAGE.        
//
#include "simplifier.hpp"
#include "directives.hpp"
#include "boolean_directives.hpp"
#include "../expressions/expression.hpp"
#include "../directives/transformer.hpp"
#include <vtil/io>
#include <vtil/utility>

// [Configuration]
// Determine the depth limit after which we start self generated signature matching and
// the properties of the LRU cache.
//
#ifndef VTIL_SYMEX_SELFGEN_SIGMATCH_DEPTH_LIM
	#define	VTIL_SYMEX_SELFGEN_SIGMATCH_DEPTH_LIM   3
#endif
#ifndef VTIL_SYMEX_LRU_CACHE_SIZE
	#define VTIL_SYMEX_LRU_CACHE_SIZE               0x18000
#endif
#ifndef VTIL_SYMEX_LRU_PRUNE_COEFF
	#define VTIL_SYMEX_LRU_PRUNE_COEFF              0.2
#endif
#ifndef VTIL_SYMEX_VERIFY
	#ifdef _DEBUG
		#define	VTIL_SYMEX_VERIFY                       1
	#else
		#define	VTIL_SYMEX_VERIFY                       0
	#endif
#endif

namespace vtil::symbolic
{
	struct join_depth_exception : std::exception
	{
		const char* what() const throw()
		{
			return "Reached the maximum join depth limit.";
		}
	};

	// Implement lookup-table based dynamic tables.
	//
	using static_directive_table_entry =  std::pair<directive::instance,        directive::instance>;
	using dynamic_directive_table_entry = std::pair<const directive::instance*, const directive::instance*>;

	using dynamic_directive_table =       std::vector<dynamic_directive_table_entry>;
	using organized_directive_table =     std::array<dynamic_directive_table, ( size_t ) math::operator_id::max>;

	template<typename T>
	static organized_directive_table build_dynamic_table( const T& container )
	{
		organized_directive_table table;
		for ( auto [table, op] : zip( table, iindices ) )
			for( auto& directive : container )
				if ( directive.first.op == ( math::operator_id ) op )
					table.emplace_back( &directive.first, &directive.second );
		return table;
	};

	static auto& get_boolean_joiners( math::operator_id op )       { static const auto tbl = build_dynamic_table( directive::boolean_joiners );             return tbl[ ( size_t ) op ]; }
	static auto& get_pack_descriptors( math::operator_id op )      { static const auto tbl = build_dynamic_table( directive::pack_descriptors );            return tbl[ ( size_t ) op ]; }
	static auto& get_join_descriptors( math::operator_id op )      { static const auto tbl = build_dynamic_table( directive::join_descriptors );            return tbl[ ( size_t ) op ]; }
	static auto& get_unpack_descriptors( math::operator_id op )    { static const auto tbl = build_dynamic_table( directive::unpack_descriptors );          return tbl[ ( size_t ) op ]; }
	static auto& get_boolean_simplifiers( math::operator_id op )   { static const auto tbl = build_dynamic_table( directive::build_boolean_simplifiers() ); return tbl[ ( size_t ) op ]; }
	static auto& get_universal_simplifiers( math::operator_id op ) { static const auto tbl = build_dynamic_table( directive::universal_simplifiers );       return tbl[ ( size_t ) op ]; }

	// Thread local simplifier state.
	//
	struct simplifier_state
	{
		// Declare the cache itself.
		//
		struct sig_hasher
		{
			size_t operator()( const expression::reference& x ) { return x->signature.hash_value.as64() ^ ( x->depth + ( uint8_t ) x->op ); }
		};
		struct cache_entry
		{
			simplifier_state& self;
			detached_queue_key<cache_entry> spec_key;

			bool is_simplified;
			expression::reference result;

			~cache_entry()
			{
				if ( spec_key.is_valid() )
					self.spec_queue.erase( &spec_key );
			}
		};
		using cache_type = lru_flatmap<expression::reference, cache_entry, sig_hasher>;
		using entry_type = typename cache_type::entry_type;

		cache_type cache = {
			VTIL_SYMEX_LRU_CACHE_SIZE,
			VTIL_SYMEX_LRU_PRUNE_COEFF
		};

		// Whether we're executing speculatively or not.
		//
		bool is_speculative = false;

		// Queue for speculativeness.
		//
		detached_queue<cache_entry> spec_queue;

		// Maximum allowed depth, once reached will reset to 0 to make all calls recursively fail.
		//
		size_t max_depth = std::numeric_limits<size_t>::max();

		struct depth_tracker
		{
			simplifier_state* s;
			depth_tracker() : s( nullptr ) {}
			depth_tracker( simplifier_state* s ) : s( s ) { s->max_depth--; }
			depth_tracker( depth_tracker&& o ) = delete;
			depth_tracker( const depth_tracker& ) = delete;
			depth_tracker& operator=( depth_tracker&& o ) = delete;
			depth_tracker& operator=( const depth_tracker& ) = delete;
			~depth_tracker() { if ( s->max_depth ) s->max_depth++; }
		};

		// Default move, no copy.
		//
		simplifier_state() {}
		simplifier_state( simplifier_state&& ) = default;
		simplifier_state( const simplifier_state& ) = delete;
		simplifier_state& operator=( simplifier_state&& ) = default;
		simplifier_state& operator=( const simplifier_state& ) = delete;

		// Resets the cache.
		//
		void reset() { cache.clear(); }

		// The main lookup&insert function.
		//
		std::tuple<cache_type::reference_wrapper<>, bool, depth_tracker> lookup( const expression::reference& ref )
		{
			// Find the bucket based on the signature.
			//
			size_t hash = sig_hasher{}( ref );
			auto* node = cache.find_node( hash );

			// If found any:
			//
			if ( node->hash == hash )
			{
				entry_type* sig_match = nullptr;
				std::optional<expression::uid_relation_table> sig_search;

				// Iteration guide for both directions:
				//
				std::array iteration_guide = {
					std::pair{ node,       &cache_type::bucket_header::low },
					std::pair{ node->high, &cache_type::bucket_header::high }
				};
				for ( auto [begin, direction] : iteration_guide )
				{
					// Begin at begin, continue at field as long as criteria is met.
					//
					for ( auto it = begin; it && it->hash == hash; it = it->*direction )
					{
						entry_type* entry = ( entry_type* ) it;
						auto& [key, v] = entry->kv;

						// Skip if depth or signature does not match.
						//
						if ( key->depth != ref->depth ||
							 key->signature != ref->signature )
							continue;

						// If same depth and identical, just return.
						//
						if ( ref->is_identical( *key ) )
						{
							// Prioritize in LRU tracking and return.
							//
							cache.prune_last( entry );
							return std::make_tuple( entry, true, this );
						}

						// If other's past depth limit and no sig was matched:
						//
						if ( !sig_match && key->depth > VTIL_SYMEX_SELFGEN_SIGMATCH_DEPTH_LIM )
						{
							// If signature matches, set as result.
							//
							if ( sig_search = key->match_to( *ref, /*false*/ true ) )
								sig_match = entry;
						}
					}
				}

				// If there is a partial match, simplify based on it.
				//
				if ( sig_match )
				{
					// Transform according to the UID table.
					//
					if ( sig_match->kv.second.is_simplified )
					{
						// Lower the priority as we'll be re-inserting the pattern.
						//
						if ( is_speculative || sig_match->lock_count )
							cache.prune_next( sig_match );
						else
						{
							sig_match->kv.first = ref;
							sig_match->kv.second.result.transform( [ & ] ( expression::delegate& exp )
							{
								if ( !exp->is_variable() )
									return;
								for ( auto& [a, b] : *sig_search )
								{
									if ( exp->is_identical( *a ) )
									{
										exp = b.make_shared();
										break;
									}
								}
							}, true, false );
							cache.prune_last( sig_match );
							return std::make_tuple( sig_match, true, this );
						}

						// Transform the saved expression and return after inserting
						// to the simplification cache.
						//
						auto result = make_const( sig_match->kv.second.result ).transform( [ & ] ( expression::delegate& exp )
						{
							if ( !exp->is_variable() )
								return;
							for ( auto& [a, b] : *sig_search )
							{
								if ( exp->is_identical( *a ) )
								{
									exp = b.make_shared();
									break;
								}
							}
						}, true, false );
						result->simplify_hint = true;

						auto ret = cache.emplace( ref, cache_entry{
							.self = *this,
							.spec_key = {},
							.is_simplified = true,
							.result = std::move( result )
												  } );
						if ( is_speculative )
							spec_queue.emplace_back( &ret->second.spec_key );
						return std::make_tuple( std::move( ret ), true, this );
					}
					// Since it won't be simplified, just return null indicator.
					//
					else
					{
						// Higher priority since it was used and return.
						//
						cache.prune_last( sig_match );
						return std::make_tuple( cache_type::reference_wrapper<>{}, true, this );
					}
				}
			}

			auto ret = cache.emplace( ref, cache_entry{
				.self = *this,
				.spec_key = {},
				.is_simplified = false,
				.result = nullptr
			} );
			if ( is_speculative )
				spec_queue.emplace_back( &ret->second.spec_key );
			return std::make_tuple( std::move( ret ), false, this );
		}

		// Begins speculative execution.
		//
		void begin_speculative( size_t* out, size_t depth )
		{
			is_speculative = true;
			*out = std::exchange( max_depth, depth );
		}

		// Ends speculative execution, returns false if failed.
		//
		bool end_speculative( size_t* in )
		{
			// Restore state.
			//
			is_speculative = false;

			// If failed, trashes all incomplete speculative entries.
			//
			if ( !std::exchange( max_depth, *in ) )
			{
				spec_queue.pop_front( &cache_entry::spec_key );

				for ( auto it = spec_queue.head; it; )
				{
					auto next = it->next;
					entry_type* entry = ptr_at<entry_type>( it, -( int64_t ) &make_null<entry_type>()->kv.second.spec_key );

					if ( entry->kv.second.is_simplified )
						spec_queue.erase( it );
					else
						cache.erase( entry );

					it = next;
				}
				return false;
			}
			// Otherwise remove marks.
			//
			else
			{
				for ( auto it = spec_queue.head; it; )
				{
					auto next = it->next;
					spec_queue.erase( it );
					it = next;
				}
				return true;
			}
		}
	};
	static thread_local simplifier_state local_state;

	// Attempts to prettify the expression given.
	//
	static bool prettify_expression( expression::reference& exp )
	{
		using namespace logger;
#if VTIL_SYMEX_SIMPLIFY_VERBOSE
		scope_padding _p( 1 );
		log<CON_CYN>( "[Prettify]  = %s\n", *exp );
#endif

		// Prettify each operand.
		//
		auto pexp = +exp;
		for ( auto* op_ptr : { &pexp->lhs, &pexp->rhs } )
		{
			if ( !op_ptr->is_valid() ) continue;
			
			// If successful, recurse.
			//
			if ( prettify_expression( *op_ptr ) )
			{
				pexp->update( false );
				simplify_expression( exp, true, false );
				return true;
			}
		}

		// Update the expression.
		//
		pexp->update( false );
		
		// Enumerate each pack descriptor:
		//
		for ( auto [dir_src, dir_dst] : get_pack_descriptors( exp->op )  )
		{
			// If we can transform the expression by the directive set:
			//
			if ( auto exp_new = transform( exp, dir_src, dir_dst ) )
			{
#if VTIL_SYMEX_SIMPLIFY_VERBOSE
				log<CON_PRP>( "[Pack] %s => %s\n", *dir_src, *dir_dst );
				log<CON_GRN>( "= %s\n", *exp );
#endif
				exp = exp_new;
				return true;
			}
		}

#if VTIL_SYMEX_SIMPLIFY_VERBOSE
		log<CON_YLW>( "= %s\n", *exp );
#endif
		return false;
	}

	// Checks if the expression can be interpreted as a vector-boolean expression.
	//
	static std::pair<bool, expression::weak_reference> match_boolean_expression( const expression::reference& exp )
	{
		switch ( exp->op )
		{
			// If constant / variable, indicate success and return self if variable:
			//
			case math::operator_id::invalid:
			{
				if ( exp->is_variable() ) return { true, exp };
				else                      return { true, nullptr };
			}

			// If bitwise not, continue from rhs.
			//
			case math::operator_id::bitwise_not:
				return match_boolean_expression( exp->rhs );

			// Bitwise OR/AND/XOR match both sides, if both were succesful
			// and had matching/null UIDs, indicate success.
			//
			case math::operator_id::bitwise_or:
			case math::operator_id::bitwise_and:
			case math::operator_id::bitwise_xor:
			{
				auto [m1, p1] = match_boolean_expression( exp->lhs );
				if ( !m1 ) return { false, nullptr };
				auto [m2, p2] = match_boolean_expression( exp->rhs );
				if ( !m2 ) return { false, nullptr };

				if ( !p2 ) return { true, p1 };
				if ( !p1 ) return { true, p2 };

				if ( p1->uid == p2->uid )
					return { true, p1 };
				else
					return { false, nullptr };
			}

			// Illegal operation, fail.
			//
			default:
				return { false, nullptr };
		}
	}

	// Attempts to normalize a vector-boolean expression into a simpler format.
	//
	static bool simplify_boolean_expression( expression::reference& exp )
	{
		// If it does not match a basic boolean expression, return false.
		//
		auto [is_match, uid_base] = match_boolean_expression( exp );
		if ( !is_match ) return false;

		// Evaluate for both states.
		//
		auto r0 = exp->evaluate( [ & ] ( auto& uid ) { return 0ull; } );
		auto r1 = exp->evaluate( [ & ] ( auto& uid ) { return ~0ull; } );

		// Calculate normal form AND/OR/XOR masks.
		//
		uint64_t and_mask = { ~( r0.known_zero() & r1.known_zero() ) };
		uint64_t or_mask = { r0.known_one() & r1.known_one() };
		uint64_t xor_mask = { r0.known_one() & r1.known_zero() };

		// Apply each mask if not no-op.
		//
		expression::reference exp_new = uid_base.make_shared();
		if ( and_mask != ~0ull )  exp_new &= expression{ and_mask, exp->size() };
		if ( xor_mask )           exp_new ^= expression{ xor_mask, exp->size() };
		if ( or_mask )            exp_new |= expression{ or_mask,  exp->size() };

		// If complexity was higher or equal, fail.
		//
		if ( exp_new->complexity >= exp->complexity ) return false;
		
		// Apply and return.
		//
		exp = std::move( exp_new );
		return true;
	}

	// Attempts to simplify the expression given, returns whether the simplification
	// succeeded or not.
	//
	static bool simplify_expression_i( expression::reference& exp, bool pretty, bool unpack )
	{
		auto& lstate = local_state;
		using namespace logger;

		// If we've reached the maximum depth, recursively fail.
		//
		if ( !lstate.max_depth )
		{
#if VTIL_SYMEX_SIMPLIFY_VERBOSE
			log<CON_CYN>( "Depth limit reached!\n" );
#endif
			return false;
		}

		// Clear lazy if not done.
		//
		if ( exp->is_lazy )
			( +exp )->is_lazy = false;

		// If not an expression, we cannot simplify further.
		//
		if ( !exp->is_expression() )
			return false;

		// If simplify hint is set, only call prettify if requested and return.
		//
		if ( exp->simplify_hint )
		{
			if ( pretty )
				prettify_expression( exp );
			return false;
		}

		// If expression has known value, return as is.
		//
		if ( exp->value.is_known() )
		{
			*+exp = expression{ exp->value.known_one(), exp->value.size() };
#if VTIL_SYMEX_SIMPLIFY_VERBOSE
			log<CON_CYN>( "= %s [By evaluation]\n", *exp );
#endif
			return true;
		}

#if VTIL_SYMEX_SIMPLIFY_VERBOSE
		// Log the input.
		//
		scope_padding _p( 1 );
		if ( !logger_state.padding ) log( "\n" );
		log( "[Input]  = %s ", *exp );
		log( "(Hash: %s)\n", exp->hash() );
#endif

		// Lookup the expression in the cache.
		//
		auto [cache, found, _g] = lstate.lookup( exp );

		// If we resolved a valid cache entry:
		//
		if ( found )
		{
			// Replace with the cached entry if simplifies.
			//
			if ( cache && cache->second.is_simplified )
			{
#if VTIL_SYMEX_SIMPLIFY_VERBOSE
				log<CON_YLW>( "= %s (From cache, Success: %d)\n", cache->second.result, cache->second.is_simplified );
#endif
				exp = cache->second.result;
				return true;
			}
#if VTIL_SYMEX_SIMPLIFY_VERBOSE
			log<CON_RED>( "Failed as directed by cache...\n" );
#endif
			return false;
		}

		// If trying to simplify resizing:
		//
		if ( exp->op == math::operator_id::ucast ||
			 exp->op == math::operator_id::cast )
		{
			// Simplify left hand side with the exact same arguments.
			//
			expression::reference exp_new = exp->lhs;
			bool simplified = simplify_expression( exp_new, pretty, unpack );
			bitcnt_t new_size = math::narrow_cast<bitcnt_t>( *exp->rhs->get() );

			// Invoke resize with failure on explicit cast:
			//
			exp_new.resize( new_size, exp->op == math::operator_id::cast, true );

			// If implicit resize failed:
			//
			if ( exp_new->size() != new_size )
			{
				// If operand was simplified, indicate success.
				//
				if ( simplified )
				{
					( +exp )->lhs = exp_new;
					( +exp )->update( false );
					cache->second.is_simplified = true;
				}
			}
			else
			{
				// If operand was simplified or if the complexity reduced, indicate success. 
				//
				if ( simplified || exp_new->complexity < exp->complexity )
				{
					exp = exp_new;
					cache->second.is_simplified = true;
				}
			}

			exp->simplify_hint = true;
			cache->second.result = exp;
			return cache->second.is_simplified;
		}

		// If expression matches a basic boolean expression, simplify through that first:
		//
		if ( simplify_boolean_expression( exp ) )
		{
			// Recurse, and indicate success.
			//
			simplify_expression( exp, pretty );
			exp->simplify_hint = true;
			cache->second.result = exp;
			cache->second.is_simplified = true;
			return true;
		}

		// Simplify operands first if not done already.
		//
		for ( auto* op_ptr : { &exp->lhs, &exp->rhs } )
		{
			// If invalid or is simplified, skip.
			//
			if ( !op_ptr->is_valid() || op_ptr->get()->simplify_hint )
				continue;

			// If we could simplify the operand:
			//
			expression::reference op_ref = *op_ptr;
			if ( simplify_expression( op_ref, false ) )
			{
				// Own the reference and relocate the pointer.
				//
				auto [exp_new, op_new] = exp.own( op_ptr );

				// Update the expression.
				//
				*op_new = op_ref;
				exp_new->update( false );

				// Recurse, and indicate success.
				//
				simplify_expression( exp, pretty );
				exp->simplify_hint = true;
				cache->second.result = exp;
				cache->second.is_simplified = true;
				return true;
			}
		}

#if VTIL_SYMEX_SIMPLIFY_VERBOSE
		// Log the bit states.
		//
		log( "[Vector] = %s\n", exp->value );
#endif

		// If reduced to a constant, replace it.
		//
		if ( exp->value.is_known() )
		{
			cache->second.result = expression{ exp->value.known_one(), exp->value.size() };
			cache->second.is_simplified = true;
			exp = cache->second.result;
#if VTIL_SYMEX_SIMPLIFY_VERBOSE
			log<CON_CYN>( "= %s [By evaluation]\n", *exp );
#endif
			return cache->second.is_simplified;
		}

		// Enumerate each universal simplifier:
		//
		for ( auto& [dir_src, dir_dst] : get_universal_simplifiers( exp->op ) )
		{
			// If we can transform the expression by the directive set:
			//
			if ( auto exp_new = transform( exp, dir_src, dir_dst ) )
			{
#if VTIL_SYMEX_SIMPLIFY_VERBOSE
				log<CON_GRN>( "[Simplify] %s => %s\n", *dir_src, *dir_dst );
				log<CON_GRN>( "= %s [By simplify directive]\n", *exp_new );
#endif
				// Recurse, set the hint and return the simplified instance.
				//
				simplify_expression( exp_new, pretty );
				exp_new->simplify_hint = true;
				cache->second.result = exp_new;
				if( cache->second.is_simplified = !exp->is_identical( *exp_new ) )
					exp = exp_new;
				return cache->second.is_simplified;
			}
		}

		// If it is a boolean expression:
		//
		if ( exp->size() == 1 )
		{
			// Enumerate each universal simplifier:
			//
			for ( auto& [dir_src, dir_dst] : get_boolean_simplifiers( exp->op ) )
			{
				// If we can transform the expression by the directive set:
				//
				if ( auto exp_new = transform( exp, dir_src, dir_dst ) )
				{
#if VTIL_SYMEX_SIMPLIFY_VERBOSE
					log<CON_GRN>( "[Simplify] %s => %s\n", *dir_src, *dir_dst );
					log<CON_GRN>( "= %s [By simplify directive]\n", *exp_new );
#endif
					// Recurse, set the hint and return the simplified instance.
					//
					simplify_expression( exp_new, pretty );
					exp_new->simplify_hint = true;
					cache->second.result = exp_new;
					if ( cache->second.is_simplified = !exp->is_identical( *exp_new ) )
						exp = exp_new;
					return cache->second.is_simplified;
				}
			}
		}

		// Declare the filter.
		//
		auto filter = [ & ] ( auto& exp_new )
		{
			if ( !lstate.is_speculative )
			{
				// If complexity was reduced already, pass.
				//
				if ( exp_new->complexity < exp->complexity )
					return true;

				// Try simplifying with maximum depth set as expression's
				// depth times two and pass if complexity was reduced.
				//
				size_t prev;
				lstate.begin_speculative( &prev, exp_new->depth * 2 );
				simplify_expression( exp_new, false );
				return lstate.end_speculative( &prev ) && exp_new->complexity < exp->complexity;
			}
			else
			{
				// If complexity was reduced already, pass.
				//
				if ( exp_new->complexity < exp->complexity )
					return true;

				// Attempt simplifying with maximum depth decremented by one,
				// fail if complexity was not reduced.
				//
				simplify_expression( exp_new, false );
				return exp_new->complexity < exp->complexity;
			}
		};

		// Enumerate each join descriptor:
		//
		for ( auto& [dir_src, dir_dst] : get_join_descriptors( exp->op ) )
		{
			// If we can transform the expression by the directive set:
			//
			if ( auto exp_new = transform( exp, dir_src, dir_dst, filter ) )
			{
#if VTIL_SYMEX_SIMPLIFY_VERBOSE
				log<CON_GRN>( "[Join] %s => %s\n", *dir_src, *dir_dst );
				log<CON_GRN>( "= %s [By join directive]\n", *exp_new );
				log<CON_YLW>( "Complexity: %lf => %lf\n", exp->complexity, exp_new->complexity );
#endif
				// Recurse, set the hint and return the simplified instance.
				//
				simplify_expression( exp_new, pretty );
				exp_new->simplify_hint = true;
				cache->second.result = exp_new;
				if ( cache->second.is_simplified = !exp->is_identical( *exp_new ) )
					exp = exp_new;
				return cache->second.is_simplified;
			}
		}

		// If it is a boolean expression:
		//
		if ( exp->size() == 1 )
		{
			// Enumerate each join descriptor:
			//
			for ( auto& [dir_src, dir_dst] : get_boolean_joiners( exp->op ) )
			{
				// If we can transform the expression by the directive set:
				//
				if ( auto exp_new = transform( exp, dir_src, dir_dst, filter ) )
				{
#if VTIL_SYMEX_SIMPLIFY_VERBOSE
					log<CON_GRN>( "[Join] %s => %s\n", *dir_src, *dir_dst );
					log<CON_GRN>( "= %s [By join directive]\n", *exp_new );
					log<CON_YLW>( "Complexity: %lf => %lf\n", exp->complexity, exp_new->complexity );
#endif
					// Recurse, set the hint and return the simplified instance.
					//
					simplify_expression( exp_new, pretty );
					exp_new->simplify_hint = true;
					cache->second.result = exp_new;
					if ( cache->second.is_simplified = !exp->is_identical( *exp_new ) )
						exp = exp_new;
					return cache->second.is_simplified;
				}
			}
		}

		// Unpack the expression if requested:
		//
		if ( unpack )
		{
			// Enumerate each unpack descriptor:
			//
			for ( auto& [dir_src, dir_dst] : get_unpack_descriptors( exp->op ) )
			{
				// If we can transform the expression by the directive set:
				//
				if ( auto exp_new = transform( exp, dir_src, dir_dst,
					 [ & ] ( auto& exp_new ) { simplify_expression( exp_new, true ); return exp_new->complexity < exp->complexity; } ) )
				{
#if VTIL_SYMEX_SIMPLIFY_VERBOSE
					log<CON_YLW>( "[Unpack] %s => %s\n", *dir_src, *dir_dst );
					log<CON_GRN>( "= %s [By unpack directive]\n", *exp_new );
#endif

					// Set the hint and return the simplified instance.
					//
					exp_new->simplify_hint = true;
					cache->second.result = exp_new;
					if ( cache->second.is_simplified = !exp->is_identical( *exp_new ) )
						exp = exp_new;
					return cache->second.is_simplified;
				}
			}
		}

		// Prettify the expression if requested.
		//
		if ( pretty )
			prettify_expression( exp );

#if VTIL_SYMEX_SIMPLIFY_VERBOSE
		// Log the output.
		//
		log( "= %s\n\n", *exp );
#endif
		return false;
	}

	// Simple routine wrapping real simplification to instrument it for any reason when needed.
	//
	bool simplify_expression( expression::reference& exp, bool pretty, bool unpack )
	{
#if VTIL_SYMEX_VERIFY
		auto previous_value = exp->xvalues();
#endif
		bool simplified = simplify_expression_i( exp, pretty, unpack );
#if VTIL_SYMEX_VERIFY
		if ( simplified )
			fassert( previous_value == exp->xvalues() );
#endif
		return simplified;
	}
};