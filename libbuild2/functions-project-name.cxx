// file      : libbuild2/functions-project-name.cxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

#include <libbuild2/function.hxx>
#include <libbuild2/variable.hxx>

using namespace std;

namespace build2
{
  void
  project_name_functions (function_map& m)
  {
    function_family f (m, "project_name");

    // Note that we must handle NULL values (relied upon by the parser
    // to provide conversion semantics consistent with untyped values).
    //
    f["string"] += [](project_name* p)
    {
      return p != nullptr ? move (*p).string () : string ();
    };

    f["base"] += [](project_name p, optional<string> ext)
    {
      return ext ? p.base (ext->c_str ()) : p.base ();
    };

    f["base"] += [](project_name p, names ext)
    {
      return p.base (convert<string> (move (ext)).c_str ());
    };

    f["extension"] += &project_name::extension;
    f["variable"]  += &project_name::variable;

    // Project name-specific overloads from builtins.
    //
    function_family b (m, "builtin");

    // Note that while we should normally handle NULL values (relied upon by
    // the parser to provide concatenation semantics consistent with untyped
    // values), the result will unlikely be what the user expected. So for now
    // we keep it a bit tighter.
    //
    b[".concat"] += [](project_name n, string s)
    {
      string r (move (n).string ());
      r += s;
      return r;
    };

    b[".concat"] += [](string s, project_name n)
    {
      s += n.string ();
      return s;
    };

    b[".concat"] += [](project_name n, names ns)
    {
      string r (move (n).string ());
      r += convert<string> (move (ns));
      return r;
    };

    b[".concat"] += [](names ns, project_name n)
    {
      string r (convert<string> (move (ns)));
      r += n.string ();
      return r;
    };
  }
}
