# Contributing

Eglantine has lots of different ways that you can contribute to its efforts, even without knowing how to write code:

- Join our [Discord](https://discord.gg/jCCZ58VExk) community
- Submit [bug reports or feature requests](https://github.com/Djihads80/Eglantine/issues)
- Improve documentation or share reproducible testing results
- Contribute to our [translation efforts](./dist/languages/README.md)

## Code Contributions

Eglantine is free, open-source, copyleft software, licensed under the terms of the [GPLv3](https://www.gnu.org/licenses/gpl-3.0.en.html). You would do well to familiarize yourself with the GPL, [its FAQ](https://www.gnu.org/licenses/gpl-faq.html), and [free software as a concept](https://www.gnu.org/philosophy/free-sw.html) before contributing. If these terms are not acceptable to you, then you shouldn't contribute.

### Policies

- We are trialing a slightly relaxed AI policy to allow high-quality contributors to use AI *responsibly*. Please read our [detailed AI policy](docs/policies/AI.md) for more info.
- Usage of any form of profanity or otherwise unsavory language is generally discouraged.
  - This is primarily because it rarely helps to actually understand what's going on.
  - Remember that your comments should be focused and actually address what's happening. With very few exceptions, expletives are actively detrimental at best.
- New code must follow the same general style as the surrounding codebase. Exceptions may be granted in certain cases.
- Maintainers reserve the right to change your patches and pull requests at will. We will try to avoid this.
  - You should generally respect all decisions made by the [code owners](docs/CODEOWNERS) in your particular subsystem.
  - However, if you feel they are overstepping or are incorrect, don't be afraid to stand your ground! Maintainers are not always correct.
- While we do *not* adhere to the terms of a formal code of conduct, you will generally be expected to respect other developers, contributors, and community members.
- You **must** have basic knowledge of [Git](https://git-scm.com/learn). Knowing how to manage your branches and follow proper fork policies is a necessity.

### Where do I start?

Anywhere you like! If you are facing an issue and want to fix it, go ahead. For new features, you are heavily encouraged to open a [feature request](https://github.com/Djihads80/Eglantine/issues) first, discussing the motivations, potential implementation, and user flow of your desired feature.

Search the [open issues](https://github.com/Djihads80/Eglantine/issues) and [pull requests](https://github.com/Djihads80/Eglantine/pulls) before starting to avoid duplicating work. For larger features/refactors, you should first express your interest in the issue to ensure another developer isn't already working on it.

### Great! Can I contribute already?

Submit a pull request to [Djihads80/Eglantine](https://github.com/Djihads80/Eglantine). You need a GitHub account; the Eden Forgejo account registration process does not apply here.

1. Fork the Eglantine repository to your GitHub account.
2. Clone your fork and create a branch for your change based on Eglantine's `nx2` branch.
3. Make a focused change, following the policies above and the [coding guidelines](docs/policies/Coding.md).
4. Review and test your changes. For code changes, follow the [build documentation](docs/Build.md) for your platform and verify the affected behavior. For documentation changes, preview the Markdown and check the instructions and links.
5. Commit your changes and push your branch to your fork.
6. Open a pull request from your branch to `Djihads80/Eglantine`, selecting `nx2` as the base branch. Explain the problem, the change, and how you verified it, and link any related issue.

Before opening a pull request, make sure your branch is up-to-date with Eglantine's `nx2` branch. Follow the [AI policy](docs/policies/AI.md), including its requirements for human-written commit messages and pull request descriptions, disclosure, review, testing, and sign-off.

## Non-code Contributions

Alongside the other contribution methods listed up top, you can also choose to contribute through documentation, organization, or community guides. These can be done either through the code contribution methods described above, or created externally and shared via our Discord community.

If you have an external tool/page that you believe would be handy to integrate/link into Eglantine, please discuss it with the developers on [Discord](https://discord.gg/jCCZ58VExk). **Do not submit vibe-coded or AI generated tools or applications**.
