### Markdown Configuration

Configuration is defined using Markdown files organized into hierarchical chapters. Chapters are defined by Markdown headers, where the header level determines the depth of nesting.

Within each chapter scope, data can be defined in two ways:

*   **Key-Value Pairs**: Using the `Key: value` format. When key-value pairs are present, all other bare text within that chapter scope is discarded.
*   **Bare Values**: If no key-value pairs exist, the chapter's entire content is treated as its value (trimmed of leading and trailing whitespace), excluding any nested sub-chapters.

**Example:**
```markdown
# Chapter 1
Key1: value1

## Subchapter 1.1
Bare value 1.1

## Subchapter 1.2
Key2: value2
```

**Result:**
- `config['Chapter 1']['Key1'] = 'value1'`
- `config['Chapter 1']['Subchapter 1.1'] = 'Bare value 1.1'`
- `config['Chapter 1']['Subchapter 1.2']['Key2'] = 'value2'`

Duplicate chapter names at the same level are not permitted.