### Configuration Layout

**Base Directory**  
`$FLOWER_BASE` is defined as `$XDG_CONFIG_HOME/flower/` or `$HOME/.config/flower/`.  
Refer to [docs/config_markdown_format.md](docs/config_markdown_format.md) for file format specifications.

#### Global Configuration
`$FLOWER_BASE/flower.md`: Contains user identity and roles.  
Managed via: `./flower setup`

```markdown
# User
Name: Daniele
Role: Software Developer
```

#### Project Configuration
Projects are organized as directories within `$FLOWER_BASE/projects/<project_id>/`.

1. **`config.md`**: Basic project metadata.  
   Managed via: `./flower project <project_id> set <key> "<value>"`
   - Keys: `Name`, `Path`, `Description`.

   ```markdown
   # Project
   Name: flower
   Path: /home/user/projects/flower
   Description: A project description
   ```

2. **`details.md`**: Optional project specifications.  
   - Chapters: `Output`, `Objectives`, `Scope`, `Constraints`, `Stakeholders`.

   ```markdown
   # Output
   The project output are csv files.
   ```

3. **`context.md`**: Project knowledge base. Chapter names must start with a fixed prefix and cannot be duplicated.  
   - Prefixes: `Fact`, `Pattern`, `Risk`, `Success metric`, `Failure sign`, `Evaluation method`, or `Rule`.

   ```markdown
   # Fact: configuration location
   Configuration always happens in directory X

   # Rule: temporary files
   Temp files are discarded automatically at process termination
   ```

4. **`resources.md`**: Resource pointers (URLs, file system paths, or code fragments).

   ```markdown
   # staging webapp url
   https://www.example.org/

   # bulk csv generator
   src/product/feeds/generator.py#generate_csv
   ```

#### Task Configuration
Tasks are organized as directories within `$FLOWER_BASE/projects/<project_id>/tasks/<task_id>/`.

1. **`config.md`**: Task metadata.  
   - Keys: `Name`, `Description`.

2. **`context.md`**: Task-specific knowledge base. Follows the same format and prefix rules as Project context.

3. **`actions.md`**: Ordered sequence of actions.  
   - Keys: `Description`, `Status` (e.g., pending, complete), `Depends` (comma-separated list of action IDs).

   ```markdown
   # A1: take water
   Description: Water can be taken using a glass.
   Status: complete

   # A2: take flour
   Description: Flour is in the cupboard.
   Status: pending

   # A3: mix water and flour
   Description: Put water and flour in a bowl.
   Status: pending
   Depends: A1, A2
   ```



