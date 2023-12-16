project = "eshetcpp"
copyright = "2023, Thomas Nixon"
author = "Thomas Nixon"

extensions = []

extensions.append("breathe")
breathe_projects = {"eshetcpp": "./_doxygen/xml"}
breathe_default_project = "eshetcpp"

extensions.append("exhale")
from textwrap import dedent

exhale_args = dict(
    containmentFolder="./api",
    rootFileName="library_root.rst",
    rootFileTitle="API",
    doxygenStripFromPath="../../include",
    createTreeView=True,
    exhaleExecutesDoxygen=True,
    exhaleDoxygenStdin=dedent(
        """
            INPUT = ../../include
            EXCLUDE_PATTERNS += *util.hpp
        """
    ),
    afterTitleDescription=dedent(
        """
        The most important class to look at is
        :class:`eshet::detail::ESHETClientActor`, which contains the
        implementation of the :type:`eshet::ESHETClient` typedef that is
        normally used.
        """
    ),
)

extensions.append("sphinx.ext.intersphinx")
intersphinx_mapping = {
    "eshet": ("https://eshet.readthedocs.io/en/latest/", None),
}

extensions.append("myst_parser")

templates_path = ["_templates"]
exclude_patterns = []

html_theme = "sphinx_rtd_theme"
html_static_path = ["_static"]

primary_domain = "cpp"
highlight_language = "cpp"
