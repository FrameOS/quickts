// The block lowering (JS_EVAL_FLAG_JSX_BLOCKS, `QJS_JSX_BLOCKS=1` for qjs):
// one call per JSX site, __frameosBlock(template, ...holes), the template a
// sealed constant. The runner runs this file under both lowerings and passes
// `blocks` as an argument when the flag is on; under the factory lowering
// __frameosBlock is never called and only the shared checks run.
declare const scriptArgs: string[];

function assert(cond: boolean, msg: string): void {
  if (!cond) throw new Error(msg);
}
const same = (a: unknown, b: unknown, msg: string) =>
  assert(JSON.stringify(a) === JSON.stringify(b), msg + ": " + JSON.stringify(a));

const __frameosFragment = Symbol("fragment");
const __frameosJsx = (type: unknown, props: any, ...children: unknown[]) => {
  const nextProps = props ?? {};
  if (children.length === 1) nextProps.children = children[0];
  else if (children.length > 1) nextProps.children = children;
  if (type === __frameosFragment) return nextProps.children ?? null;
  return typeof type === "function" ? type(nextProps) : { type, props: nextProps };
};
// The reference expansion of a template: what the factory would have made.
const calls: Array<[unknown[], unknown[]]> = [];
const __frameosBlock = (template: unknown[], ...holes: unknown[]) => {
  calls.push([template, holes]);
  const fill = (v: unknown): unknown =>
    typeof v === "number" ? (v < 0 ? holes[-1 - v] : v) : Array.isArray(v) ? node(v) : v;
  const node = (t: any[]): unknown => {
    let props: any = null;
    if (t[1] !== null) {
      props = {};
      for (let i = 0; i < t[1].length; i += 2) {
        const value = fill(t[1][i + 1]);
        if (t[1][i] === null) Object.assign(props, value);
        else props[t[1][i]] = value;
      }
    }
    const children: unknown[] = [];
    for (let i = 2; i < t.length; i++) children.push(fill(t[i]));
    return __frameosJsx(t[0] === null ? __frameosFragment : fill(t[0]), props, ...children);
  };
  return node(template);
};
const blocks = scriptArgs[1] === "blocks";
const last = () => calls[calls.length - 1];
const shape = (holes: unknown[]) => holes.map((h) => (typeof h === "function" ? "fn" : h));

// --- the shared checks: both lowerings make the same elements --------------
const x = 5;
const Card = (p: any) => ({ card: p });
const o = { a: 1, b: 2 };
const el: any = (
  <box a={x} b="y" flag n={10} s={"str"} sum={1 + 1} {...o} icon=<i /> t={`t`}>
    text {x} <Card k={1}>{x > 0 && <b>in</b>}</Card>{"lit"}{42}{}{/* c */}&amp;
  </box>
);
assert(el.type === "box", "tag");
same(
  [el.props.a, el.props.b, el.props.flag, el.props.n, el.props.s, el.props.sum, el.props.a, el.props.t],
  [1, 2, true, 10, "str", 2, 1, "t"],
  "props (the spread after b overrides it)",
);
assert(el.props.icon.type === "i", "element-valued attribute");
const kids = el.props.children;
assert(kids.length === 7, "children: " + kids.length);
same([kids[0], kids[1], kids[2], kids[4], kids[5], kids[6]], ["text ", 5, " ", "lit", 42, "&"], "children");
assert(kids[3].card.k === 1 && kids[3].card.children.type === "b", "component child");
const frag: any = <>{x}<a /></>;
assert(frag.length === 2 && frag[1].type === "a", "fragment");
const none: any = <p />;
same(Object.keys(none.props), [], "no attributes, no props");

assert(blocks === calls.length > 0, "the lowering the runner asked for ran: " + calls.length);
if (!blocks) {
  console.log("ok");
} else {
  // --- the template -----------------------------------------------------
  calls.length = 0;
  <box a={x} b="y" flag n={10} s={"str"} sum={1 + 1} {...o} icon=<i /> t={`t`}>
    text {x} <Card k={1}>{x > 0 && <b>in</b>}</Card>{"lit"}{42}{}{/* c */}&amp;
  </box>;
  // the inner site (`<b>in</b>` inside a `{}`) is a call of its own, made first
  same(calls[0][0], ["b", null, "in"], "a nested site");
  same(calls[0][1], [], "a static site has no holes");
  same(
    calls[1][0],
    [
      "box",
      ["a", -1, "b", "y", "flag", true, "n", 10, "s", "str", "sum", -2, null, -3, "icon", ["i", null], "t", -4],
      "text ", -5, " ", [-6, ["k", 1], -7], "lit", 42, "&",
    ],
    "the template",
  );
  same(shape(calls[1][1]), [5, 2, { a: 1, b: 2 }, "t", 5, "fn", { type: "b", props: { children: "in" } }], "the holes, in source order");
  assert(calls.length === 2, "two sites");

  // a literal alone in braces is static, anything more is a hole
  calls.length = 0;
  <v a={1} b={1.5} c={1e3} d={0x10} e={"s"} f={'q'} g={-1} h={1n} i={`t`} j={(1)} k={1 + 0} />;
  same(last()[0], ["v", ["a", 1, "b", 1.5, "c", 1000, "d", 16, "e", "s", "f", "q", "g", -1, "h", -2, "i", -3, "j", -4, "k", -5]], "static literals");
  same(last()[1].map(String), ["-1", "1", "t", "1", "1"], "the rest are holes");
  assert(typeof last()[1][1] === "bigint", "a BigInt literal is a hole, not folded");
  <c>{1}{"a"}{-1}{x}</c>;
  same(last()[0], ["c", null, 1, "a", -1, -2], "static children");

  // sealed: the same object every evaluation, not extensible, elements
  // read-only (through bytecode `length` stays writable, as a tagged
  // template's does; the elements and the identity are what a cache needs)
  const sealed = (t: unknown) =>
    !Object.isExtensible(t) && Object.getOwnPropertyDescriptor(t, "0")?.writable === false;
  const site = () => <r />;
  site(); const t1 = last()[0]; site(); const t2 = last()[0];
  assert(t1 === t2, "one template per site");
  assert(sealed(t1), "sealed");
  <q a="1"><s /></q>;
  assert(sealed(last()[0]) && sealed(last()[0][1]) && sealed(last()[0][2]), "props and nested nodes sealed too");
  assert((() => { "use strict"; try { (t1 as any)[0] = "w"; return false; } catch (e) { return true; } })(), "an element is read-only");
  site(); assert(last()[0] !== (<r />, last()[0]), "another site, another template");

  // components, members, fragments, spreads, bare `{expr}` in attribute position
  const Ns = { In: (p: any) => p };
  <Ns.In z={x}><Card /></Ns.In>;
  same(last()[0], [-1, ["z", -2], [-3, null]], "member tag, nested component");
  same(shape(last()[1]), ["fn", 5, "fn"], "tag holes come first");
  <><a />{x}text</>;
  same(last()[0], [null, null, ["a", null], -1, "text"], "fragment template");
  <w {...o} q={2} {...{ r: 3 }} />;
  same(last()[0], ["w", [null, -1, "q", 2, null, -2]], "spreads keep their place");
  same(last()[1], [{ a: 1, b: 2 }, { r: 3 }], "spread holes");
  <e data-id="x" xlink:href="y" title="Tom &amp; Jerry" />;
  same(last()[0], ["e", ["data-id", "x", "xlink:href", "y", "title", "Tom & Jerry"]], "attribute names and entities");
  const list = [1, 2].map((n) => <li key={n}>{n}</li>);
  assert(list[1].props.children === 2 && calls[calls.length - 1][0] === calls[calls.length - 2][0], "one template for a mapped site");
  console.log("ok");
}
