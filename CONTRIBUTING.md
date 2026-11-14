# Contributing

We welcome contributions: issues, fixes and new features. Before you send
a change, build it and run the quick tests
(`ctest --test-dir build -L quick`). For more data, refer to
[docs/building.md](docs/building.md).

Write all technical documentation in ASD-STE100 Simplified Technical
English. Follow [docs/STYLE.md](docs/STYLE.md).

## The contributor agreement

D-Modem is offered under the AGPL-3.0 and, by the D-Modem Authors, under
commercial licenses ([LICENSING.md](LICENSING.md)), and a few of its files
under other licenses named there.  For contributions to be part of all of
them, each one comes with this agreement.
By submitting a contribution (a pull request, a patch or
any other code or text meant for inclusion), you agree that:

1. The contribution is your own original work, or you otherwise have
   the right to submit it under these terms, and to your knowledge it
   infringes no one's rights.  If your employer has rights in what you
   create, you have its permission to contribute, or it has waived those
   rights.
2. You assign to D-Modem all copyright in your contribution, worldwide,
   for the whole term of those rights.  "D-Modem" here is the D-Modem
   organization (<https://github.com/D-Modem>), whose rights its owner,
   Neo-Desktop, holds on its behalf; the "D-Modem Authors" in the
   project's copyright notices are D-Modem in this sense.  Where a right
   can't be assigned (moral rights, for example), you waive it, or where
   it can't be waived, you agree not to assert it against D-Modem or
   those who receive the project from it.  If the assignment is not
   effective anywhere, you instead grant D-Modem there an exclusive,
   perpetual, worldwide, no-charge, royalty-free, irrevocable license to
   do anything the copyright holder could do with your contribution,
   including licensing it to others.
3. D-Modem may license your contribution, and works based on it, under
   the AGPL-3.0, the licenses LICENSING.md names and under any other
   terms, commercial licenses included. It licenses your contribution
   back to you: a perpetual, worldwide, non-exclusive, no-charge,
   royalty-free, irrevocable license to use, copy, modify, publicly
   display and perform, sublicense and distribute it, on any terms.
4. You grant D-Modem, and those who receive the project from it, a
   perpetual, worldwide, non-exclusive, no-charge, royalty-free,
   irrevocable license under any patent claims of yours that your
   contribution would infringe, to make, use, sell and otherwise
   transfer it as part of the project.
5. Your contribution is provided "as is", without warranties or
   conditions of any kind.

Please add this line to each commit you contribute (`git commit -s`
does it):

    Signed-off-by: Your Name <your@email>

It says that you agree to the above for that commit.  A contribution
without it cannot be merged.

A check on each pull request looks for that line, with the commit
author's email, on every commit.  If it's missing, `git rebase --signoff
main` (or `git commit --amend -s` for the last commit) adds it; then
force-push.  Pull requests from the D-Modem Authors themselves (the
repository's owner and the D-Modem organization's members) are not
checked.
