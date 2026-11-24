# C++ class conventions

- Use `src/core/halfhourindex.hpp` as the reference for class documentation and organization.
- Give each project-owned class and struct a high-level Doxygen `@brief` explaining its responsibility. This includes nested types, implementation helpers, and test fixtures; forward declarations do not need duplicate documentation.
- Within each visibility block, group declarations under descriptive uppercase section comments such as `// CONSTRUCTOR`, `// DESTRUCTOR`, `// METHODS`, `// OPERATORS`, and `// MEMBERS`. Label only sections that contain declarations, and keep existing member documentation with its declaration.
- Capitalize class and struct members unless their names begin with an underscore. Use PascalCase for ordinary names (for example, `Path` and `InputWindowSeconds`); existing uppercase constants and leading-underscore members may retain their style. Parameters and local variables may remain lowercase.
- Update all callers when renaming a member. Preserve serialized keys, binary layouts, generated code, third-party APIs, and names required by framework overrides.
- Apply these conventions to new code and to existing code being modified. Do not edit vendored dependencies or generated sources to enforce them.
