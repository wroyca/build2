// file      : libbuild2/types-parsers.hxx -*- C++ -*-
// license   : MIT; see accompanying LICENSE file

// CLI parsers, included into the generated source files.
//

#ifndef LIBBUILD2_TYPES_PARSERS_HXX
#define LIBBUILD2_TYPES_PARSERS_HXX

#include <libbuild2/types.hxx>

#include <libbuild2/types-parsers.hxx>

namespace build2
{
  namespace build
  {
    namespace cli
    {
      class scanner;

      template <typename T>
      struct parser;

      template <>
      struct parser<path>
      {
        static void
        parse (path&, bool&, scanner&);

        static void
        merge (path& b, const path& a) {b = a;}
      };

      template <>
      struct parser<dir_path>
      {
        static void
        parse (dir_path&, bool&, scanner&);

        static void
        merge (dir_path& b, const dir_path& a) {b = a;}
      };
    }
  }
}

#endif // LIBBUILD2_TYPES_PARSERS_HXX