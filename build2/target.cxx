// file      : build2/target.cxx -*- C++ -*-
// copyright : Copyright (c) 2014-2016 Code Synthesis Ltd
// license   : MIT; see accompanying LICENSE file

#include <build2/target>

#include <butl/filesystem>

#include <build2/scope>
#include <build2/search>
#include <build2/algorithm>
#include <build2/diagnostics>

using namespace std;

namespace build2
{
  // target_type
  //
  bool target_type::
  is_a (const target_type& tt) const
  {
    for (const target_type* p (this); p != nullptr; p = p->base)
      if (*p == tt)
        return true;

    return false;
  }

  // target_state
  //
  static const char* target_state_[] = {
    "unknown", "unchanged", "postponed", "changed", "failed", "group"};

  ostream&
  operator<< (ostream& os, target_state ts)
  {
    return os << target_state_[static_cast<uint8_t> (ts)];
  }

  // recipe
  //
  const recipe empty_recipe;
  const recipe noop_recipe (&noop_action);
  const recipe default_recipe (&default_action);
  const recipe group_recipe (&group_action);

  // target
  //

  void target::
  recipe (action_type a, recipe_type r)
  {
    assert (a > action || !recipe_);

    bool override (a == action && recipe_); // See action::operator<.

    // Only noop_recipe can be overridden.
    //
    if (override)
    {
      recipe_function** f (recipe_.target<recipe_function*> ());
      assert (f != nullptr && *f == &noop_action);
    }

    action = a;
    recipe_ = move (r);

    raw_state = target_state::unknown;

    // If this is a noop recipe, then mark the target unchanged so that we
    // don't waste time executing the recipe.
    //
    if (recipe_function** f = recipe_.target<recipe_function*> ())
    {
      if (*f == &noop_action)
        raw_state = target_state::unchanged;
    }

    // This one is tricky: we don't want to reset the dependents count
    // if we are merely overriding with a "stronger" recipe.
    //
    if (!override)
      dependents = 0;
  }

  void target::
  reset (action_type)
  {
    prerequisite_targets.clear ();
  }

  group_view target::
  group_members (action_type) const
  {
    assert (false); // Not a group or doesn't expose its members.
    return group_view {nullptr, 0};
  }

  scope& target::
  base_scope () const
  {
    // If this target is from the src tree, use its out directory to find
    // the scope.
    //
    return scopes.find (out.empty () ? dir : out);
  }

  scope& target::
  root_scope () const
  {
    // This is tricky to cache so we do the lookup for now.
    //
    scope* r (base_scope ().root_scope ());
    assert (r != nullptr);
    return *r;
  }

  pair<lookup, size_t> target::
  find_original (const variable& var) const
  {
    pair<lookup, size_t> r (lookup (), 0);

    ++r.second;
    if (auto p = vars.find (var))
      r.first = lookup (p, &vars);

    if (!r.first)
    {
      ++r.second;
      if (group != nullptr)
      {
        if (auto p = group->vars.find (var))
          r.first = lookup (p, &group->vars);
      }
    }

    // Delegate to scope's find_original().
    //
    if (!r.first)
    {
      auto p (base_scope ().find_original (
                var,
                &type (),
                &name,
                group != nullptr ? &group->type () : nullptr,
                group != nullptr ? &group->name : nullptr));

      r.first = move (p.first);
      r.second = r.first ? r.second + p.second : p.second;
    }

    return r;
  }

  value& target::
  append (const variable& var)
  {
    // Note that here we want the original value without any overrides
    // applied.
    //
    lookup l (find_original (var).first);

    if (l.defined () && l.belongs (*this)) // Existing var in this target.
      return const_cast<value&> (*l); // Ok since this is original.

    value& r (assign (var)); // NULL.

    if (l.defined ())
      r = *l; // Copy value (and type) from the outer scope.

    return r;
  }

  // target_set
  //
  target_set targets;

  auto target_set::
  find (const target_key& k, tracer& trace) const -> iterator
  {
    iterator i (map_.find (k));

    if (i != end ())
    {
      target& t (**i);

      // Update the extension if the existing target has it unspecified.
      //
      const string* ext (k.ext);
      if (t.ext != ext)
      {
        l5 ([&]{
            diag_record r (trace);
            r << "assuming target " << t << " is the same as the one with ";
            if (ext == nullptr)
              r << "unspecified extension";
            else if (ext->empty ())
              r << "no extension";
            else
              r << "extension " << *ext;
          });

        if (ext != nullptr)
          t.ext = ext;
      }
    }

    return i;
  }

  pair<target&, bool> target_set::
  insert (const target_type& tt,
          dir_path dir,
          dir_path out,
          string name,
          const string* ext,
          tracer& trace)
  {
    iterator i (find (target_key {&tt, &dir, &out, &name, ext}, trace));
    bool r (i == end ());

    if (r)
    {
      unique_ptr<target> pt (
        tt.factory (tt, move (dir), move (out), move (name), ext));
      i = map_.emplace (
        make_pair (target_key {&tt, &pt->dir, &pt->out, &pt->name, pt->ext},
                   move (pt))).first;
    }

    return pair<target&, bool> (**i, r);
  }

  ostream&
  to_stream (ostream& os, const target_key& k, uint16_t ev)
  {
    // If the name is empty, then we want to print the directory
    // inside {}, e.g., dir{bar/}, not bar/dir{}.
    //
    bool n (!k.name->empty ());

    if (n)
    {
      // Avoid printing './' in './{...}'
      //
      if (stream_verb (os) < 2)
        os << diag_relative (*k.dir, false);
      else
        os << *k.dir;
    }

    os << k.type->name << '{';

    if (n)
    {
      os << *k.name;

      // If the extension derivation function is NULL, then it means this
      // target type doesn't use extensions.
      //
      if (k.type->extension != nullptr)
      {
        // For verbosity level 0 we don't print the extension. For 1 we print
        // it if there is one. For 2 we print 'foo.?' if it hasn't yet been
        // assigned and 'foo.' if it is assigned as "no extension" (empty).
        //
        if (ev > 0 && (ev > 1 || (k.ext != nullptr && !k.ext->empty ())))
        {
          os << '.' << (k.ext != nullptr ? *k.ext : "?");
        }
      }
      else
        assert (k.ext == nullptr);
    }
    else
      os << *k.dir;

    os << '}';

    // If this target is from src, print its out.
    //
    if (!k.out->empty ())
      os << '@' << diag_relative (*k.out, false); // Don't print './'.

    return os;
  }

  ostream&
  operator<< (ostream& os, const target_key& k)
  {
    if (auto p = k.type->print)
      p (os, k);
    else
      to_stream (os, k, stream_verb (os));

    return os;
  }

  // path_target
  //
  void path_target::
  derive_path (const char* de, const char* np, const char* ns)
  {
    string n;

    if (np != nullptr)
      n += np;

    n += name;

    if (ns != nullptr)
      n += ns;

    // Update the extension. See also search_existing_file() if updating
    // anything here.
    //
    assert (de == nullptr || type ().extension != nullptr);

    if (ext == nullptr)
    {
      // If the target type has the extension function then try that first.
      // The reason for preferring it over what's been provided by the caller
      // is that this function will often use the 'extension' variable which
      // the user can use to override extensions.
      //
      if (auto f = type ().extension)
        ext = f (key (), base_scope ()); // Already from the pool.

      if (ext == nullptr)
      {
        if (de != nullptr)
          ext = &extension_pool.find (de);
        else
          fail << "no default extension for target " << *this;
      }
    }

    // Add the extension.
    //
    if (!ext->empty ())
    {
      n += '.';
      n += *ext;
    }

    path_type p (dir / path_type (move (n)));
    const path_type& ep (path ());

    if (ep.empty ())
      path (p);
    else if (p != ep)
      fail << "path mismatch for target " << *this <<
        info << "assigned '" << ep << "'" <<
        info << "derived  '" << p << "'";
  }

  // file_target
  //
  timestamp file::
  load_mtime () const
  {
    const path_type& f (path ());
    assert (!f.empty ());
    return file_mtime (f);
  }

  // Search functions.
  //

  target*
  search_target (const prerequisite_key& pk)
  {
    // The default behavior is to look for an existing target in the
    // prerequisite's directory scope.
    //
    return search_existing_target (pk);
  }

  target*
  search_file (const prerequisite_key& pk)
  {
    // First see if there is an existing target.
    //
    if (target* t = search_existing_target (pk))
      return t;

    // Then look for an existing file in the src tree.
    //
    return pk.tk.dir->relative () ? search_existing_file (pk) : nullptr;
  }

  static target*
  search_alias (const prerequisite_key& pk)
  {
    // For an alias we don't want to silently create a target since it
    // will do nothing and it most likely not what the user intended.
    //
    target* t (search_existing_target (pk));

    if (t == nullptr)
      fail << "no explicit target for prerequisite " << pk;

    return t;
  }

  const string*
  target_extension_null (const target_key&, scope&)
  {
    return nullptr;
  }

  const string*
  target_extension_assert (const target_key&, scope&)
  {
    assert (false); // Attempt to obtain the default extension.
    throw failed ();
  }

  void
  target_print_0_ext_verb (ostream& os, const target_key& k)
  {
    uint16_t v (stream_verb (os));
    to_stream (os, k, v < 2 ? 0 : v); // Remap 1 to 0.
  }

  void
  target_print_1_ext_verb (ostream& os, const target_key& k)
  {
    uint16_t v (stream_verb (os));
    to_stream (os, k, v < 1 ? 1 : v); // Remap 0 to 1.
  }

  // type info
  //

  const target_type target::static_type
  {
    "target",
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    &search_target,
    false
  };

  const target_type mtime_target::static_type
  {
    "mtime_target",
    &target::static_type,
    nullptr,
    nullptr,
    nullptr,
    &search_target,
    false
  };

  const target_type path_target::static_type
  {
    "path_target",
    &mtime_target::static_type,
    nullptr,
    nullptr,
    nullptr,
    &search_target,
    false
  };

  template <typename T>
  static target*
  file_factory (const target_type&,
                dir_path d,
                dir_path o,
                string n,
                const string* e)
  {
    // The file target type doesn't imply any extension. So if one wasn't
    // specified, set it to empty rather than unspecified. In other words, we
    // always treat file{foo} as file{foo.}.
    //
    return new T (move (d),
                  move (o),
                  move (n),
                  (e != nullptr ? e : &extension_pool.find ("")));
  }

  constexpr const char file_ext_var[] = "extension";
  constexpr const char file_ext_def[] = "";

  const target_type file::static_type
  {
    "file",
    &path_target::static_type,
    &file_factory<file>,
    &target_extension_var<file_ext_var, file_ext_def>,
    &target_print_1_ext_verb, // Print extension even at verbosity level 0.
    &search_file,
    false
  };

  const target_type alias::static_type
  {
    "alias",
    &target::static_type,
    &target_factory<alias>,
    nullptr, // Extension not used.
    nullptr,
    &search_alias,
    false
  };

  const target_type dir::static_type
  {
    "dir",
    &alias::static_type,
    &target_factory<dir>,
    nullptr, // Extension not used.
    nullptr,
    &search_alias,
    false
  };

  const target_type fsdir::static_type
  {
    "fsdir",
    &target::static_type,
    &target_factory<fsdir>,
    nullptr, // Extension not used.
    nullptr,
    &search_target,
    false
  };

  static const string*
  buildfile_target_extension (const target_key& tk, scope&)
  {
    // If the name is special 'buildfile', then there is no extension,
    // otherwise it is .build.
    //
    return &extension_pool.find (*tk.name == "buildfile" ? "" : "build");
  }

  const target_type buildfile::static_type
  {
    "buildfile",
    &file::static_type,
    &file_factory<buildfile>,
    &buildfile_target_extension,
    nullptr,
    &search_file,
    false
  };

  const target_type doc::static_type
  {
    "doc",
    &file::static_type,
    &file_factory<doc>,
    &target_extension_var<file_ext_var, file_ext_def>, // Same as file.
    &target_print_1_ext_verb, // Same as file.
    &search_file,
    false
  };

  static target*
  man_factory (const target_type&,
               dir_path d,
               dir_path o,
               string n,
               const string* e)
  {
    if (e == nullptr)
      fail << "man target '" << n << "' must include extension (man section)";

    return new man (move (d), move (o), move (n), e);
  }

  const target_type man::static_type
  {
    "man",
    &doc::static_type,
    &man_factory,
    &target_extension_null, // Should be specified explicitly (see factory).
    &target_print_1_ext_verb, // Print extension even at verbosity level 0.
    &search_file,
    false
  };

  constexpr const char man1_ext[] = "1";
  const target_type man1::static_type
  {
    "man1",
    &man::static_type,
    &file_factory<man1>,
    &target_extension_fix<man1_ext>,
    &target_print_0_ext_verb, // Fixed extension, no use printing.
    &search_file,
    false
  };
}
